/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  Copyright (c) 2001-2003 Cluster File Systems, Inc.
 *
 *   This file is part of Lustre, http://www.lustre.org.
 *
 *   Lustre is free software; you can redistribute it and/or
 *   modify it under the terms of version 2 of the GNU General Public
 *   License as published by the Free Software Foundation.
 *
 *   Lustre is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Lustre; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * Config API
 *
 */

#define DEBUG_SUBSYSTEM S_CLASS
#ifdef __KERNEL__
#include <linux/kmod.h>   /* for request_module() */
#include <linux/module.h>
#include <linux/obd_class.h>
#include <linux/random.h>
#include <linux/slab.h>
#include <linux/pagemap.h>
#else
#include <liblustre.h>
#include <linux/obd_class.h>
#include <linux/obd.h>
#endif
#include <linux/lustre_log.h>
#include <linux/lprocfs_status.h>
#include <libcfs/list.h>


/* Create a new device and set the type, name and uuid.  If
 * successful, the new device can be accessed by either name or uuid.
 */
int class_attach(struct lustre_cfg *lcfg)
{
        struct obd_type *type;
        struct obd_device *obd;
        char *typename, *name, *namecopy, *uuid;
        int rc, len, cleanup_phase = 0;

        if (!lcfg->lcfg_inllen1 || !lcfg->lcfg_inlbuf1) {
                CERROR("No type passed!\n");
                RETURN(-EINVAL);
        }
        if (lcfg->lcfg_inlbuf1[lcfg->lcfg_inllen1 - 1] != 0) {
                CERROR("Type not nul terminated!\n");
                RETURN(-EINVAL);
        }
        typename = lcfg->lcfg_inlbuf1;

        if (!lcfg->lcfg_dev_namelen || !lcfg->lcfg_dev_name) {
                CERROR("No name passed!\n");
                RETURN(-EINVAL);
        }
        if (lcfg->lcfg_dev_name[lcfg->lcfg_dev_namelen - 1] != 0) {
                CERROR("Name not nul terminated!\n");
                RETURN(-EINVAL);
        }
        name = lcfg->lcfg_dev_name;

        if (!lcfg->lcfg_inllen2 || !lcfg->lcfg_inlbuf2) {
                CERROR("No UUID passed!\n");
                RETURN(-EINVAL);
        }
        if (lcfg->lcfg_inlbuf2[lcfg->lcfg_inllen2 - 1] != 0) {
                CERROR("UUID not nul terminated!\n");
                RETURN(-EINVAL);
        }
        uuid = lcfg->lcfg_inlbuf2;

        CDEBUG(D_IOCTL, "attach type %s name: %s uuid: %s\n",
               MKSTR(lcfg->lcfg_inlbuf1),
               MKSTR(lcfg->lcfg_dev_name), MKSTR(lcfg->lcfg_inlbuf2));

        /* find the type */
        type = class_get_type(typename);
        if (!type) {
                CERROR("OBD: unknown type: %s\n", typename);
                RETURN(-ENODEV);
        }
        cleanup_phase = 1;  /* class_put_type */

        len = strlen(name) + 1;
        OBD_ALLOC(namecopy, len);
        if (!namecopy) 
                GOTO(out, rc = -ENOMEM);
        memcpy(namecopy, name, len);
        cleanup_phase = 2; /* free obd_name */

        obd = class_newdev(type, namecopy);
        if (obd == NULL) {
                /* Already exists or out of obds */
                CERROR("Can't create device %s\n", name);
                GOTO(out, rc = -EEXIST);
        }
        cleanup_phase = 3;  /* class_release_dev */

        INIT_LIST_HEAD(&obd->obd_exports);
        obd->obd_num_exports = 0;
        spin_lock_init(&obd->obd_dev_lock);
        spin_lock_init(&obd->obd_osfs_lock);
        obd->obd_osfs_age = jiffies - 1000 * HZ;

        /* XXX belongs in setup not attach  */
        /* recovery data */
        init_timer(&obd->obd_recovery_timer);
        spin_lock_init(&obd->obd_processing_task_lock);
        init_waitqueue_head(&obd->obd_next_transno_waitq);
        INIT_LIST_HEAD(&obd->obd_recovery_queue);
        INIT_LIST_HEAD(&obd->obd_delayed_reply_queue);

        spin_lock_init(&obd->obd_uncommitted_replies_lock);
        INIT_LIST_HEAD(&obd->obd_uncommitted_replies);

        len = strlen(uuid);
        if (len >= sizeof(obd->obd_uuid)) {
                CERROR("uuid must be < "LPSZ" bytes long\n",
                       sizeof(obd->obd_uuid));
                GOTO(out, rc = -EINVAL);
        }
        memcpy(obd->obd_uuid.uuid, uuid, len);

        /* do the attach */
        if (OBP(obd, attach)) {
                rc = OBP(obd,attach)(obd, sizeof *lcfg, lcfg);
                if (rc)
                        GOTO(out, rc = -EINVAL);
        }

        /* The attach is our first obd reference */
        atomic_set(&obd->obd_refcount, 1);

        obd->obd_attached = 1;
        type->typ_refcnt++;
        CDEBUG(D_IOCTL, "OBD: dev %d attached type %s\n",
               obd->obd_minor, typename);
        RETURN(0);
 out:
        switch (cleanup_phase) {
        case 3:
                class_release_dev(obd);
        case 2:
                OBD_FREE(namecopy, strlen(namecopy) + 1);
        case 1:
                class_put_type(type);
        }
        return rc;
}

int class_setup(struct obd_device *obd, struct lustre_cfg *lcfg)
{
        int err = 0;
        struct obd_export *exp;
        ENTRY;

        LASSERT(obd == (obd_dev + obd->obd_minor));

        /* have we attached a type to this device? */
        if (!obd->obd_attached) {
                CERROR("Device %d not attached\n", obd->obd_minor);
                RETURN(-ENODEV);
        }
        
        if (obd->obd_set_up) {
                CERROR("Device %d already setup (type %s)\n",
                       obd->obd_minor, obd->obd_type->typ_name);
                RETURN(-EEXIST);
        }

        /* is someone else setting us up right now? (attach inits spinlock) */
        spin_lock(&obd->obd_dev_lock);
        if (obd->obd_starting) {
                spin_unlock(&obd->obd_dev_lock);
                CERROR("Device %d setup in progress (type %s)\n",
                       obd->obd_minor, obd->obd_type->typ_name);
                RETURN(-EEXIST);
        }
        /* just leave this on forever.  I can't use obd_set_up here because
           other fns check that status, and we're not actually set up yet. */
        obd->obd_starting = 1;  
        spin_unlock(&obd->obd_dev_lock);

        exp = class_new_export(obd);
        if (exp == NULL)
                RETURN(err);
        memcpy(&exp->exp_client_uuid, &obd->obd_uuid,
               sizeof(exp->exp_client_uuid));
        obd->obd_self_export = exp;
        class_export_put(exp);

        err = obd_setup(obd, sizeof(*lcfg), lcfg);
        if (err)
                GOTO(err_exp, err);

        obd->obd_type->typ_refcnt++;
        obd->obd_set_up = 1;
        CDEBUG(D_IOCTL, "finished setup of obd %s (uuid %s)\n",
               obd->obd_name, obd->obd_uuid.uuid);
        
        RETURN(0);

err_exp:
        class_unlink_export(obd->obd_self_export);
        obd->obd_self_export = NULL;
        obd->obd_starting = 0;
        RETURN(err);
}

static int __class_detach(struct obd_device *obd)
{
        int err = 0;

        if (OBP(obd, detach)) 
                err = OBP(obd,detach)(obd);
        
        if (obd->obd_name) {
                OBD_FREE(obd->obd_name, strlen(obd->obd_name)+1);
                obd->obd_name = NULL;
        } else {
                CERROR("device %d: no name at detach\n", obd->obd_minor);
        }
        
        LASSERT(OBT(obd));
        /* Attach took type refcount */
        obd->obd_type->typ_refcnt--;
        class_put_type(obd->obd_type);
        class_release_dev(obd);
        return (err);
}

int class_detach(struct obd_device *obd, struct lustre_cfg *lcfg)
{
        ENTRY;
        if (obd->obd_set_up) {
                CERROR("OBD device %d still set up\n", obd->obd_minor);
                RETURN(-EBUSY);
        }

        spin_lock(&obd->obd_dev_lock);
        if (!obd->obd_attached) {
                spin_unlock(&obd->obd_dev_lock);
                CERROR("OBD device %d not attached\n", obd->obd_minor);
                RETURN(-ENODEV);
        }
        obd->obd_attached = 0;
        spin_unlock(&obd->obd_dev_lock);
        
        CDEBUG(D_IOCTL, "detach on obd %s (uuid %s)\n",
               obd->obd_name, obd->obd_uuid.uuid);

        class_decref(obd);
        RETURN(0);
}

static void dump_exports(struct obd_device *obd)
{
        struct obd_export *exp, *n;

        list_for_each_entry_safe(exp, n, &obd->obd_exports, exp_obd_chain) {
                struct ptlrpc_reply_state *rs;
                struct ptlrpc_reply_state *first_reply = NULL;
                int                        nreplies = 0;

                list_for_each_entry (rs, &exp->exp_outstanding_replies,
                                     rs_exp_list) {
                        if (nreplies == 0)
                                first_reply = rs;
                        nreplies++;
                }

                CDEBUG(D_IOCTL, "%s: %p %s %d %d %d: %p %s\n",
                       obd->obd_name, exp, exp->exp_client_uuid.uuid,
                       atomic_read(&exp->exp_refcount),
                       exp->exp_failed, nreplies, first_reply,
                       nreplies > 3 ? "..." : "");
        }
}

int class_cleanup(struct obd_device *obd, struct lustre_cfg *lcfg)
{
        int err = 0;
        char *flag;

        ENTRY;
        OBD_RACE(OBD_FAIL_LDLM_RECOV_CLIENTS);

        if (!obd->obd_set_up) {
                CERROR("Device %d not setup\n", obd->obd_minor);
                RETURN(-ENODEV);
        }

        spin_lock(&obd->obd_dev_lock);
        if (obd->obd_stopping) {
                spin_unlock(&obd->obd_dev_lock);
                CERROR("OBD %d already stopping\n", obd->obd_minor);
                RETURN(-ENODEV);
        }
        /* Leave this on forever */
        obd->obd_stopping = 1;
        spin_unlock(&obd->obd_dev_lock);

        if (lcfg->lcfg_inlbuf1) {
                for (flag = lcfg->lcfg_inlbuf1; *flag != 0; flag++)
                        switch (*flag) {
                        case 'F':
                                obd->obd_force = 1;
                                break;
                        case 'A':
                                obd->obd_fail = 1;
                                obd->obd_no_transno = 1;
                                LCONSOLE_WARN("Failing %s by user command\n",
                                       obd->obd_name);
                                /* Set the obd readonly if we can */
                                if (OBP(obd, iocontrol))
                                        obd_iocontrol(OBD_IOC_SET_READONLY,
                                                      obd->obd_self_export,
                                                      0, NULL, NULL);
                                break;
                        default:
                                CERROR("unrecognised flag '%c'\n",
                                       *flag);
                        }
        }
        
        /* The two references that should be remaining are the
         * obd_self_export and the attach reference. */
        if (atomic_read(&obd->obd_refcount) > 2) {
                if (!(obd->obd_fail || obd->obd_force)) {
                        CERROR("OBD %s is still busy with %d references\n"
                               "You should stop active file system users,"
                               " or use the --force option to cleanup.\n",  
                               obd->obd_name, atomic_read(&obd->obd_refcount));
                        dump_exports(obd);
                        GOTO(out, err = -EBUSY);
                }
                CDEBUG(D_IOCTL, "%s: forcing exports to disconnect: %d\n",
                       obd->obd_name, atomic_read(&obd->obd_refcount));
                dump_exports(obd);
                class_disconnect_exports(obd);
        }

        LASSERT(obd->obd_self_export);
        if (obd->obd_self_export) {
               /* mds_precleanup will clean up the lov (and osc's)*/
               err = obd_precleanup(obd);
               if (err)
                       GOTO(out, err);
               obd->obd_self_export->exp_flags |= 
                       (obd->obd_fail ? OBD_OPT_FAILOVER : 0) |
                       (obd->obd_force ? OBD_OPT_FORCE : 0);
               class_unlink_export(obd->obd_self_export);
               obd->obd_self_export = NULL;
        }

        obd->obd_set_up = 0;
        obd->obd_type->typ_refcnt--;
        RETURN(0);
out:
        /* Allow a failed cleanup to try again. */
        obd->obd_stopping = 0;
        RETURN(err);
}

void class_decref(struct obd_device *obd)
{            
        if (atomic_dec_and_test(&obd->obd_refcount)) {
                int err;
                CDEBUG(D_IOCTL, "finishing cleanup of obd %s (%s)\n",
                       obd->obd_name, obd->obd_uuid.uuid);
                LASSERT(!obd->obd_attached);
                if (obd->obd_stopping) {
                        /* If we're not stopping, we never set up */
                        err = obd_cleanup(obd);
                        if (err) 
                                CERROR("Cleanup returned %d\n", err);
                }
                err = __class_detach(obd);
                if (err) 
                        CERROR("Detach returned %d\n", err);
        }
}               

LIST_HEAD(lustre_profile_list);

struct lustre_profile *class_get_profile(char * prof)
{
        struct lustre_profile *lprof;

        list_for_each_entry(lprof, &lustre_profile_list, lp_list) {
                if (!strcmp(lprof->lp_profile, prof)) {
                        RETURN(lprof);
                }
        }
        RETURN(NULL);
}

int class_add_profile(int proflen, char *prof, int osclen, char *osc,
                      int mdclen, char *mdc)
{
        struct lustre_profile *lprof;
        int err = 0;

        OBD_ALLOC(lprof, sizeof(*lprof));
        if (lprof == NULL)
                GOTO(out, err = -ENOMEM);
        INIT_LIST_HEAD(&lprof->lp_list);

        LASSERT(proflen == (strlen(prof) + 1));
        OBD_ALLOC(lprof->lp_profile, proflen);
        if (lprof->lp_profile == NULL)
                GOTO(out, err = -ENOMEM);
        memcpy(lprof->lp_profile, prof, proflen);

        LASSERT(osclen == (strlen(osc) + 1));
        OBD_ALLOC(lprof->lp_osc, osclen);
        if (lprof->lp_profile == NULL)
                GOTO(out, err = -ENOMEM);
        memcpy(lprof->lp_osc, osc, osclen);

        if (mdclen > 0) {
                LASSERT(mdclen == (strlen(mdc) + 1));
                OBD_ALLOC(lprof->lp_mdc, mdclen);
                if (lprof->lp_mdc == NULL)
                        GOTO(out, err = -ENOMEM);
                memcpy(lprof->lp_mdc, mdc, mdclen);
        }

        list_add(&lprof->lp_list, &lustre_profile_list);

out:
        RETURN(err);
}

void class_del_profile(char *prof)
{
        struct lustre_profile *lprof;

        lprof = class_get_profile(prof);
        if (lprof) {
                list_del(&lprof->lp_list);
                OBD_FREE(lprof->lp_profile, strlen(lprof->lp_profile) + 1);
                OBD_FREE(lprof->lp_osc, strlen(lprof->lp_osc) + 1);
                if (lprof->lp_mdc)
                        OBD_FREE(lprof->lp_mdc, strlen(lprof->lp_mdc) + 1);
                OBD_FREE(lprof, sizeof *lprof);
        }
}

int class_process_config(struct lustre_cfg *lcfg)
{
        struct obd_device *obd;
        char str[PTL_NALFMT_SIZE];
        int err;

        LASSERT(lcfg && !IS_ERR(lcfg));

        CDEBUG(D_IOCTL, "processing cmd: %x\n", lcfg->lcfg_command);

        /* Commands that don't need a device */
        switch(lcfg->lcfg_command) {
        case LCFG_ATTACH: {
                err = class_attach(lcfg);
                GOTO(out, err);
        }
        case LCFG_ADD_UUID: {
                CDEBUG(D_IOCTL, "adding mapping from uuid %s to nid "LPX64
                       " (%s), nal %x\n", lcfg->lcfg_inlbuf1, lcfg->lcfg_nid,
                       portals_nid2str(lcfg->lcfg_nal, lcfg->lcfg_nid, str),
                       lcfg->lcfg_nal);

                err = class_add_uuid(lcfg->lcfg_inlbuf1, lcfg->lcfg_nid,
                                     lcfg->lcfg_nal);
                GOTO(out, err);
        }
        case LCFG_DEL_UUID: {
                CDEBUG(D_IOCTL, "removing mappings for uuid %s\n",
                       lcfg->lcfg_inlbuf1 == NULL ? "<all uuids>" :
                       lcfg->lcfg_inlbuf1);

                err = class_del_uuid(lcfg->lcfg_inlbuf1);
                GOTO(out, err);
        }
        case LCFG_MOUNTOPT: {
                CDEBUG(D_IOCTL, "mountopt: profile %s osc %s mdc %s\n",
                       lcfg->lcfg_inlbuf1, lcfg->lcfg_inlbuf2,
                       lcfg->lcfg_inlbuf3);
                /* set these mount options somewhere, so ll_fill_super
                 * can find them. */
                err = class_add_profile(lcfg->lcfg_inllen1, lcfg->lcfg_inlbuf1,
                                        lcfg->lcfg_inllen2, lcfg->lcfg_inlbuf2,
                                        lcfg->lcfg_inllen3, lcfg->lcfg_inlbuf3);
                GOTO(out, err);
        }
        case LCFG_DEL_MOUNTOPT: {
                CDEBUG(D_IOCTL, "mountopt: profile %s\n", lcfg->lcfg_inlbuf1);
                /* set these mount options somewhere, so ll_fill_super
                 * can find them. */
                class_del_profile(lcfg->lcfg_inlbuf1);
                GOTO(out, err = 0);
        }
        case LCFG_SET_TIMEOUT: {
                CDEBUG(D_IOCTL, "changing lustre timeout from %d to %d\n",
                       obd_timeout,
                       lcfg->lcfg_num);
                obd_timeout = lcfg->lcfg_num;
                GOTO(out, err = 0);
        }
        case LCFG_SET_UPCALL: {
                CDEBUG(D_IOCTL, "setting lustre ucpall to: %s\n",
                       lcfg->lcfg_inlbuf1);
                if (lcfg->lcfg_inllen1 > sizeof obd_lustre_upcall)
                        GOTO(out, err = -EINVAL);
                memcpy(obd_lustre_upcall, lcfg->lcfg_inlbuf1,
                       lcfg->lcfg_inllen1);
                GOTO(out, err = 0);
        }
        }

        /* Commands that require a device */
        obd = class_name2obd(lcfg->lcfg_dev_name);
        if (obd == NULL) {
                if (lcfg->lcfg_dev_name == NULL)
                        CERROR("this lcfg command requires a device name\n");
                else
                        CERROR("no device for: %s\n", lcfg->lcfg_dev_name);

                GOTO(out, err = -EINVAL);
        }

        switch(lcfg->lcfg_command) {
        case LCFG_SETUP: {
                err = class_setup(obd, lcfg);
                GOTO(out, err);
        }
        case LCFG_DETACH: {
                err = class_detach(obd, lcfg);
                GOTO(out, err = 0);
        }
        case LCFG_CLEANUP: {
                err = class_cleanup(obd, lcfg);
                GOTO(out, err = 0);
        }
        default: {
                CERROR("Unknown command: %d\n", lcfg->lcfg_command);
                GOTO(out, err = -EINVAL);

        }
        }
out:
        return err;
}

static int class_config_llog_handler(struct llog_handle * handle,
                                     struct llog_rec_hdr *rec, void *data)
{
        struct config_llog_instance *cfg = data;
        int cfg_len = rec->lrh_len;
        char *cfg_buf = (char*) (rec + 1);
        int rc = 0;
        ENTRY;
        if (rec->lrh_type == OBD_CFG_REC) {
                char *buf;
                struct lustre_cfg *lcfg;
                char *old_name = NULL;
                int old_len = 0;
                char *old_uuid = NULL;
                int old_uuid_len = 0;
                char *inst_name = NULL;
                int inst_len = 0;

                rc = lustre_cfg_getdata(&buf, cfg_len, cfg_buf, 1);
                if (rc)
                        GOTO(out, rc);
                lcfg = (struct lustre_cfg* ) buf;

                if (cfg && cfg->cfg_instance && lcfg->lcfg_dev_name) {
                        inst_len = strlen(lcfg->lcfg_dev_name) +
                                strlen(cfg->cfg_instance) + 2;
                        OBD_ALLOC(inst_name, inst_len);
                        if (inst_name == NULL)
                                GOTO(out, rc = -ENOMEM);
                        sprintf(inst_name, "%s-%s", lcfg->lcfg_dev_name,
                                cfg->cfg_instance);
                        old_name = lcfg->lcfg_dev_name;
                        old_len = lcfg->lcfg_dev_namelen;
                        lcfg->lcfg_dev_name = inst_name;
                        lcfg->lcfg_dev_namelen = strlen(inst_name) + 1;
                }

                if (cfg && lcfg->lcfg_command == LCFG_ATTACH) {
                        old_uuid = lcfg->lcfg_inlbuf2;
                        old_uuid_len = lcfg->lcfg_inllen2;

                        lcfg->lcfg_inlbuf2 = (char*)&cfg->cfg_uuid.uuid;
                        lcfg->lcfg_inllen2 = sizeof(cfg->cfg_uuid);
                }

                rc = class_process_config(lcfg);

                if (old_name) {
                        lcfg->lcfg_dev_name = old_name;
                        lcfg->lcfg_dev_namelen = old_len;
                        OBD_FREE(inst_name, inst_len);
                }

                if (old_uuid) {
                        lcfg->lcfg_inlbuf2 = old_uuid;
                        lcfg->lcfg_inllen2 = old_uuid_len;
                }

                lustre_cfg_freedata(buf, cfg_len);
        } else if (rec->lrh_type == PTL_CFG_REC) {
                struct portals_cfg *pcfg = (struct portals_cfg *)cfg_buf;
                if (pcfg->pcfg_command ==NAL_CMD_REGISTER_MYNID &&
                    cfg->cfg_local_nid != PTL_NID_ANY) {
                        pcfg->pcfg_nid = cfg->cfg_local_nid;
                }

                rc = libcfs_nal_cmd(pcfg);
        }
out:
        RETURN(rc);
}

int class_config_parse_llog(struct llog_ctxt *ctxt, char *name,
                            struct config_llog_instance *cfg)
{
        struct llog_handle *llh;
        int rc, rc2;
        ENTRY;

        rc = llog_create(ctxt, &llh, NULL, name);
        if (rc)
                RETURN(rc);

        rc = llog_init_handle(llh, LLOG_F_IS_PLAIN, NULL);
        if (rc)
                GOTO(parse_out, rc);

        rc = llog_process(llh, class_config_llog_handler, cfg, NULL);
parse_out:
        rc2 = llog_close(llh);
        if (rc == 0)
                rc = rc2;

        RETURN(rc);

}

int class_config_dump_handler(struct llog_handle * handle,
                              struct llog_rec_hdr *rec, void *data)
{
        int cfg_len = rec->lrh_len;
        char *cfg_buf = (char*) (rec + 1);
        int rc = 0;
        ENTRY;
        if (rec->lrh_type == OBD_CFG_REC) {
                char *buf;
                struct lustre_cfg *lcfg;

                rc = lustre_cfg_getdata(&buf, cfg_len, cfg_buf, 1);
                if (rc)
                        GOTO(out, rc);
                lcfg = (struct lustre_cfg* ) buf;

                CDEBUG(D_INFO, "lcfg command: %x\n", lcfg->lcfg_command);
                if (lcfg->lcfg_dev_name)
                        CDEBUG(D_INFO, "     devname: %s\n",
                               lcfg->lcfg_dev_name);
                if (lcfg->lcfg_flags)
                        CDEBUG(D_INFO, "       flags: %x\n", lcfg->lcfg_flags);
                if (lcfg->lcfg_nid)
                        CDEBUG(D_INFO, "         nid: "LPX64"\n",
                               lcfg->lcfg_nid);
                if (lcfg->lcfg_nal)
                        CDEBUG(D_INFO, "         nal: %x\n", lcfg->lcfg_nal);
                if (lcfg->lcfg_num)
                        CDEBUG(D_INFO, "         nal: %x\n", lcfg->lcfg_num);
                if (lcfg->lcfg_inlbuf1)
                        CDEBUG(D_INFO, "     inlbuf1: %s\n",lcfg->lcfg_inlbuf1);
                if (lcfg->lcfg_inlbuf2)
                        CDEBUG(D_INFO, "     inlbuf2: %s\n",lcfg->lcfg_inlbuf2);
                if (lcfg->lcfg_inlbuf3)
                        CDEBUG(D_INFO, "     inlbuf3: %s\n",lcfg->lcfg_inlbuf3);
                if (lcfg->lcfg_inlbuf4)
                        CDEBUG(D_INFO, "     inlbuf4: %s\n",lcfg->lcfg_inlbuf4);

                lustre_cfg_freedata(buf, cfg_len);
        } else if (rec->lrh_type == PTL_CFG_REC) {
                struct portals_cfg *pcfg = (struct portals_cfg *)cfg_buf;

                CDEBUG(D_INFO, "pcfg command: %d\n", pcfg->pcfg_command);
                if (pcfg->pcfg_nal)
                        CDEBUG(D_INFO, "         nal: %x\n",
                               pcfg->pcfg_nal);
                if (pcfg->pcfg_gw_nal)
                        CDEBUG(D_INFO, "      gw_nal: %x\n",
                               pcfg->pcfg_gw_nal);
                if (pcfg->pcfg_nid)
                        CDEBUG(D_INFO, "         nid: "LPX64"\n",
                               pcfg->pcfg_nid);
                if (pcfg->pcfg_nid2)
                        CDEBUG(D_INFO, "         nid: "LPX64"\n",
                               pcfg->pcfg_nid2);
                if (pcfg->pcfg_nid3)
                        CDEBUG(D_INFO, "         nid: "LPX64"\n",
                               pcfg->pcfg_nid3);
                if (pcfg->pcfg_misc)
                        CDEBUG(D_INFO, "         nid: %d\n",
                               pcfg->pcfg_misc);
                if (pcfg->pcfg_id)
                        CDEBUG(D_INFO, "          id: %x\n",
                               pcfg->pcfg_id);
                if (pcfg->pcfg_flags)
                        CDEBUG(D_INFO, "       flags: %x\n",
                               pcfg->pcfg_flags);
        } else {
                CERROR("unhandled lrh_type: %#x\n", rec->lrh_type);
                rc = -EINVAL;
        }
out:
        RETURN(rc);
}

int class_config_dump_llog(struct llog_ctxt *ctxt, char *name,
                           struct config_llog_instance *cfg)
{
        struct llog_handle *llh;
        int rc, rc2;
        ENTRY;

        rc = llog_create(ctxt, &llh, NULL, name);
        if (rc)
                RETURN(rc);

        rc = llog_init_handle(llh, LLOG_F_IS_PLAIN, NULL);
        if (rc)
                GOTO(parse_out, rc);

        rc = llog_process(llh, class_config_dump_handler, cfg, NULL);
parse_out:
        rc2 = llog_close(llh);
        if (rc == 0)
                rc = rc2;

        RETURN(rc);

}

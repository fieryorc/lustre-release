/* proc_lustre.c manages /proc/lustre/obd. 
 *
 * Copyright (c) 2001 Rumi Zahir <rumi.zahir@intel.com>
 *
 * This code is issued under the GNU General Public License.
 * See the file COPYING in this distribution
 *
 * OBD devices materialize in /proc as a directory:
 *              /proc/lustre/obd/<number>
 * when /dev/obd<number> is opened. When the device is closed, the 
 * directory entry disappears. 
 * 
 * For each open OBD device, code in this file also creates a file
 * named <status>. "cat /proc/lustre/obd/<number>/status" gives 
 * information about the OBD device's configuration.
 * The class driver manages the "status" entry.
 *
 * Other logical drivers can create their own entries. For example,
 * the obdtrace driver creates /proc/lustre/obd/<obdid>/stats entry.
 *
 * This file defines three functions 
 *   proc_lustre_register_obd_device() - called at device attach time
 *   proc_lustre_release_obd_device() - called at detach
 *               proc_lustre_remove_obd_entry() 
 * that dynamically create/delete /proc/lustre/obd entries:
 *
 *     proc_lustre_register_obd_device() registers an obd device,
 *     and, if this is the first OBD device, creates /proc/lustre/obd.
 *
 *     proc_lustre_release_obd_device() removes device information
 *     from /proc/lustre/obd, and if this is the last OBD device
 *     removes  /proc/lustre/obd.
 *
 *     proc_lustre_remove_obd_entry() removes a
 *     /proc/lustre/obd/<obdid>/ entry by name. This is the only
 *     function that is exported to other modules. 
 */

#define EXPORT_SYMTAB
#include <linux/config.h>
#include <linux/module.h>
#include <linux/version.h>
#include <linux/proc_fs.h>

#define DEBUG_SUBSYSTEM S_CLASS

#include <linux/obd_support.h>
#include <linux/obd_class.h>

#ifdef CONFIG_PROC_FS
extern struct proc_dir_entry proc_root;
static struct proc_dir_entry *proc_lustre_dir_entry = 0;
static struct proc_dir_entry *proc_lustre_obd_dir_entry = 0;


static int read_lustre_status(char *page, char **start, off_t offset,
			      int count, int *eof, void *data)
{
	struct obd_device * obddev = (struct obd_device *) data;
	int p;

	p = sprintf(&page[0], "device=%d\n", obddev->obd_minor);
	p += sprintf(&page[0], "name=%s\n", MKSTR(obddev->obd_name));
	p += sprintf(&page[0], "uuid=%s\n", obddev->obd_uuid); 
        p += sprintf(&page[p], "attached=1\n");
        p += sprintf(&page[0], "type=%s\n", MKSTR(obddev->obd_type->typ_name));
	
        if  (obddev->obd_flags & OBD_SET_UP) {
                p += sprintf(&page[p], "setup=1\n");
        }
        
        /* print exports */
        {
                struct list_head * lh;
                struct obd_export * export=0;
                
                lh = &obddev->obd_exports;
                while ((lh = lh->next) != &obddev->obd_exports) {
                        p += sprintf(&page[p],
                                     ((export==0) ? ", connections(" : ",") );
                        export = list_entry(lh, struct obd_export, export_chain);
                        p += sprintf(&page[p], "%p", export);
                } 
                if (export!=0) { /* there was at least one export */
                        p += sprintf(&page[p], ")");
                }
        }
        
        p += sprintf(&page[p], "\n");

	/* Compute eof and return value */
	if (offset + count >= p) {
		*eof=1;
		return (p - offset);
	}
	return count;
}

struct proc_dir_entry *
proc_lustre_register_obd_device(struct obd_device *obd)
{
	char obdname[32];
	struct proc_dir_entry *obd_dir;
	struct proc_dir_entry *obd_status = 0;

	if (!proc_lustre_dir_entry) {
		proc_lustre_dir_entry = proc_mkdir("lustre", &proc_root);
		if (IS_ERR(proc_lustre_dir_entry))
			return 0;
	
		proc_lustre_obd_dir_entry = 
			proc_mkdir("devices", proc_lustre_dir_entry);
		if (IS_ERR(proc_lustre_obd_dir_entry))
			return 0;
	}
	sprintf(obdname, "%s", obd->obd_name);
	obd_dir =  proc_mkdir(obdname, proc_lustre_obd_dir_entry);

        if (obd_dir) 
		obd_status = create_proc_entry("status", S_IRUSR | S_IFREG, 
                                               obd_dir);

	if (obd_status) {
		obd_status->read_proc = read_lustre_status;
		obd_status->data = (void*) obd;
	}

	return obd_dir;
}

void proc_lustre_remove_obd_entry(const char* name, struct obd_device *obd)
{
	struct proc_dir_entry *obd_entry = 0;
	struct proc_dir_entry *obd_dir = obd->obd_proc_entry;
	
	remove_proc_entry(name, obd_dir);

	while (obd_dir->subdir==0) {
		/* if we removed last entry in this directory,
		 * then remove parent directory unless this
		 * is /proc itself
		 */
		if (obd_dir == &proc_root) 
			break;
			
		obd_entry = obd_dir;
		obd_dir = obd_dir->parent;

		/* If /proc/lustre/obd/foo or /proc/lustre/obd or
		 * /proc/lustre is being removed, then reset
		 * internal variables
		 */

		if (obd_entry == obd->obd_proc_entry)
			obd->obd_proc_entry=0; /* /proc/lustre/obd/foo */
		else 
			if (obd_entry == proc_lustre_obd_dir_entry)
				proc_lustre_obd_dir_entry=0;
			else 
				if (obd_entry == proc_lustre_dir_entry) 
					proc_lustre_dir_entry=0;

		remove_proc_entry(obd_entry->name, obd_dir);
	}
}

void proc_lustre_release_obd_device(struct obd_device *obd)
{
	proc_lustre_remove_obd_entry("status", obd);
}


#else  /* CONFIG_PROC_FS */

struct proc_dir_entry *proc_lustre_register_obd_device(struct obd_device *obd)
{
	return 0;
}

void proc_lustre_remove_obd_entry(const char* name, struct obd_device *obd) {}
void proc_lustre_release_obd_device(struct obd_device *obd) {}

#endif   /* CONFIG_PROC_FS */
     				   
EXPORT_SYMBOL(proc_lustre_remove_obd_entry);












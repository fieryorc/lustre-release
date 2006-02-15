#define DEBUG_SUBSYSTEM S_CLASS
#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif

#include <mach/mach_types.h>
#include <string.h>
#include <sys/file.h>
#include <sys/conf.h>
#include <miscfs/devfs/devfs.h>

#include <libcfs/libcfs.h>
#include <obd_support.h>
#include <obd_class.h>
#include <lprocfs_status.h>

#ifndef OBD_MAX_IOCTL_BUFFER
#define OBD_MAX_IOCTL_BUFFER 8192
#endif

/* buffer MUST be at least the size of obd_ioctl_hdr */
int obd_ioctl_getdata(char **buf, int *len, void *arg)
{
        struct obd_ioctl_hdr *hdr;
        struct obd_ioctl_data *data;
        int err = 0;
        int offset = 0;
        ENTRY;

	hdr = (struct obd_ioctl_hdr *)arg;
        if (hdr->ioc_version != OBD_IOCTL_VERSION) {
                CERROR("Version mismatch kernel vs application\n");
                RETURN(-EINVAL);
        }

        if (hdr->ioc_len > OBD_MAX_IOCTL_BUFFER) {
                CERROR("User buffer len %d exceeds %d max buffer\n",
                       hdr->ioc_len, OBD_MAX_IOCTL_BUFFER);
                RETURN(-EINVAL);
        }

        if (hdr->ioc_len < sizeof(struct obd_ioctl_data)) {
                CERROR("OBD: user buffer too small for ioctl (%d)\n", hdr->ioc_len);
                RETURN(-EINVAL);
        }

        /* XXX allocate this more intelligently, using kmalloc when
         * appropriate */
        OBD_VMALLOC(*buf, hdr->ioc_len);
        if (*buf == NULL) {
                CERROR("Cannot allocate control buffer of len %d\n",
                       hdr->ioc_len);
                RETURN(-EINVAL);
        }
        *len = hdr->ioc_len;
        data = (struct obd_ioctl_data *)*buf;

	bzero(*buf, hdr->ioc_len);
	memcpy(*buf, (void *)arg, sizeof(struct obd_ioctl_data));
	if (data->ioc_inlbuf1)
		err = copy_from_user(&data->ioc_bulk[0], (void *)data->ioc_inlbuf1,
				     hdr->ioc_len - ((void *)&data->ioc_bulk[0] - (void *)data));

        if (obd_ioctl_is_invalid(data)) {
                CERROR("ioctl not correctly formatted\n");
                return -EINVAL;
        }

        if (data->ioc_inllen1) {
                data->ioc_inlbuf1 = &data->ioc_bulk[0];
                offset += size_round(data->ioc_inllen1);
        }

        if (data->ioc_inllen2) {
                data->ioc_inlbuf2 = &data->ioc_bulk[0] + offset;
                offset += size_round(data->ioc_inllen2);
        }

        if (data->ioc_inllen3) {
                data->ioc_inlbuf3 = &data->ioc_bulk[0] + offset;
                offset += size_round(data->ioc_inllen3);
        }

        if (data->ioc_inllen4) {
                data->ioc_inlbuf4 = &data->ioc_bulk[0] + offset;
        }

        EXIT;
        return 0;
}

/*
 * cfs pseudo device
 */
extern struct cfs_psdev_ops          obd_psdev_ops;

static int
obd_class_open(dev_t dev, int flags, int devtype, struct proc *p)
{
	if (obd_psdev_ops.p_open != NULL)
		return obd_psdev_ops.p_open(0, NULL);
	return -EPERM;
}

/*  closing /dev/obd */
static int
obd_class_release(dev_t dev, int flags, int mode, struct proc *p)
{
	if (obd_psdev_ops.p_close != NULL)
		return obd_psdev_ops.p_close(0, NULL);
	return -EPERM;
}

static int
obd_class_ioctl(dev_t dev, u_long cmd, caddr_t arg, int flag, struct proc *p)
{
	int err = 0;
	ENTRY;

	if (!is_suser())
		RETURN (-EPERM);
	if (obd_psdev_ops.p_ioctl != NULL)
		err = obd_psdev_ops.p_ioctl(NULL, cmd, (void *)arg);
	else
		err = -EPERM;

	RETURN(err);
}

static struct cdevsw obd_psdevsw = {
	obd_class_open,
	obd_class_release,
	NULL,
	NULL,
	obd_class_ioctl,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
};

cfs_psdev_t obd_psdev = {
	-1,
	NULL,
	"obd",
	&obd_psdevsw
};

int class_procfs_init(void)
{
	return 0;
}

int class_procfs_clean(void)
{
	return 0;
}

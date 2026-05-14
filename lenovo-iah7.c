#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/printk.h>
#include <linux/dmi.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/device.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("o0kam1");
MODULE_DESCRIPTION("Lenovo Legion Y9000X (2022) 82TF/IAH7 laptop extras");

static bool force;
module_param(force, bool, 0440);
MODULE_PARM_DESC(
    force,
    "Force loading this module even if model or BIOS does not match.");

bool ec_readonly = true;
module_param(ec_readonly, bool, 0440);
MODULE_PARM_DESC(
    ec_readonly,
    "Only read from embedded controller but do not write or change settings.");

struct model_config {
    struct {
        // Super I/O Configuration Registers
        // 7.15 General Control (GCTRL)
        // General Control (GCTRL)
        // (see EC Interface Registers  and 6.2 Plug and Play Configuration (PNPCFG)) in datasheet
        // note: these are in two places saved
        // in EC Interface Registers  and in super io configuration registers
        // Chip ID
        u16 ECHIPID1;
        u16 ECHIPID2;
        // Chip Version
        u16 ECHIPVER;
        u16 ECDEBUG;

        // Lenovo Custom OEM extension
        // Firmware of ITE can be extended by
        // custom program using its own "variables"
        // These are the offsets to these "variables"
        u16 EXT_FAN1_RPM_LSB;
        u16 EXT_FAN1_RPM_MSB;
        u16 EXT_FAN2_RPM_LSB;
        u16 EXT_FAN2_RPM_MSB;
    } ec_register_offset;
};

static const struct model_config module_jycn = {
    .ec_register_offset = {
        .ECHIPID1 = 0x2000,
        .ECHIPID2 = 0x2001,
        .ECHIPVER = 0x2002,
        .ECDEBUG = 0x2003,
        .EXT_FAN1_RPM_LSB = 0xC406,
        .EXT_FAN1_RPM_MSB = 0xC407,
        .EXT_FAN2_RPM_LSB = 0xC408,
        .EXT_FAN2_RPM_MSB = 0xC409,
    }
};

static const struct dmi_system_id iah7_dmi_ids[] = {
    {
        .ident = "Lenovo Legion Y9000X (2022) 82TF/IAH7",
        .matches = {
            DMI_MATCH(DMI_SYS_VENDOR, "LENOVO"),
            DMI_MATCH(DMI_PRODUCT_NAME, "82TF"),
            DMI_MATCH(DMI_BIOS_VERSION, "JYCN"),
        },
        .driver_data = (void*) &module_jycn,
    }
};

/* ================================= */
/* EC RAM Access with port-mapped IO */
/* ================================= */

/*
 * See datasheet of e.g. IT8502E/F/G, e.g.
 * 6.2 Plug and Play Configuration (PNPCFG)
 *
 * Depending on configured BARDSEL register
 * the ports
 *   ECRAM_PORTIO_ADDR_PORT and
 *   ECRAM_PORTIO_DATA_PORT
 * are configured.
 *
 * By performing IO on these ports one can
 * read/write to registers in the EC.
 *
 * "To access a register of PNPCFG, write target index to
 *  address port and access this PNPCFG register via
 *  data port" [datasheet, 6.2 Plug and Play Configuration]
 */

// IO ports used to write to communicate with embedded controller
// Start of used ports
#define ECRAM_PORTIO_START_PORT 0x4E
// Number of used ports
#define ECRAM_PORTIO_PORTS_SIZE 2
// Port used to specify address in EC RAM to read/write
// 0x4E/0x4F is the usual port for IO super controller
// 0x2E/0x2F also common (ITE can also be configured to use these)
#define ECRAM_PORTIO_ADDR_PORT 0x4E
// Port to send/receive the value to write/read
#define ECRAM_PORTIO_DATA_PORT 0x4F
// Name used to request ports
#define ECRAM_PORTIO_NAME "iah7"

struct ecram_portio {
    /* protects read/write to EC RAM performed
     * as a certain sequence of outb, inb
     * commands on the IO ports. There can
     * be at most one.
     */
    struct mutex io_port_mutex;
};

static ssize_t ecram_portio_init(struct ecram_portio* ec_portio) {
    if (!request_region(ECRAM_PORTIO_START_PORT, ECRAM_PORTIO_PORTS_SIZE,
                        ECRAM_PORTIO_NAME)) {
        pr_info("Cannot init ecram_portio the %x ports starting at %x\n",
                ECRAM_PORTIO_PORTS_SIZE, ECRAM_PORTIO_START_PORT);
        return -ENODEV;
    }
    //pr_info("Reserved %x ports starting at %x\n", ECRAM_PORTIO_PORTS_SIZE, ECRAM_PORTIO_START_PORT);
    mutex_init(&ec_portio->io_port_mutex);
    return 0;
}

static void ecram_portio_exit(struct ecram_portio* ec_portio) {
    release_region(ECRAM_PORTIO_START_PORT, ECRAM_PORTIO_PORTS_SIZE);
}

/* Read a byte from the EC RAM.
 *
 * Return status because of commong signature for alle
 * methods to access EC RAM.
 */
static ssize_t ecram_portio_read(struct ecram_portio* ec_portio, u16 offset,
                                 u8* value) {
    mutex_lock(&ec_portio->io_port_mutex);

    outb(0x2E, ECRAM_PORTIO_ADDR_PORT);
    outb(0x11, ECRAM_PORTIO_DATA_PORT);
    outb(0x2F, ECRAM_PORTIO_ADDR_PORT);
    // TODO: no explicit cast between types seems to be sometimes
    // done and sometimes not
    outb((u8) ((offset >> 8) & 0xFF), ECRAM_PORTIO_DATA_PORT);

    outb(0x2E, ECRAM_PORTIO_ADDR_PORT);
    outb(0x10, ECRAM_PORTIO_DATA_PORT);
    outb(0x2F, ECRAM_PORTIO_ADDR_PORT);
    outb((u8) (offset & 0xFF), ECRAM_PORTIO_DATA_PORT);

    outb(0x2E, ECRAM_PORTIO_ADDR_PORT);
    outb(0x12, ECRAM_PORTIO_DATA_PORT);
    outb(0x2F, ECRAM_PORTIO_ADDR_PORT);
    *value = inb(ECRAM_PORTIO_DATA_PORT);

    mutex_unlock(&ec_portio->io_port_mutex);
    return 0;
}

/* Write a byte to the EC RAM.
 *
 * Return status because of commong signature for alle
 * methods to access EC RAM.
 */
static ssize_t ecram_portio_write(struct ecram_portio* ec_portio, u16 offset,
                                  u8 value) {
    mutex_lock(&ec_portio->io_port_mutex);

    outb(0x2E, ECRAM_PORTIO_ADDR_PORT);
    outb(0x11, ECRAM_PORTIO_DATA_PORT);
    outb(0x2F, ECRAM_PORTIO_ADDR_PORT);
    // TODO: no explicit cast between types seems to be sometimes
    // done and sometimes not
    outb((u8) ((offset >> 8) & 0xFF), ECRAM_PORTIO_DATA_PORT);

    outb(0x2E, ECRAM_PORTIO_ADDR_PORT);
    outb(0x10, ECRAM_PORTIO_DATA_PORT);
    outb(0x2F, ECRAM_PORTIO_ADDR_PORT);
    outb((u8) (offset & 0xFF), ECRAM_PORTIO_DATA_PORT);

    outb(0x2E, ECRAM_PORTIO_ADDR_PORT);
    outb(0x12, ECRAM_PORTIO_DATA_PORT);
    outb(0x2F, ECRAM_PORTIO_ADDR_PORT);
    outb(value, ECRAM_PORTIO_DATA_PORT);

    mutex_unlock(&ec_portio->io_port_mutex);
    // TODO: remove this
    //pr_info("Writing %d to addr %x\n", value, offset);
    return 0;
}

static u8 ec_read(struct ecram_portio* ec_portio, u16 ecram_offset) {
    u8 value;
    int err;

    err = ecram_portio_read(ec_portio, ecram_offset, &value);
    if (err)
        pr_info("Error reading EC RAM at 0x%x.\n", ecram_offset);
    return value;
}

static void ec_write(struct ecram_portio* ec_portio, u16 ecram_offset, u8 value) {
    int err;

    if (ec_readonly) {
        pr_info("Skipping writing EC RAM to 0x%x: Read-Only.\n",
                ecram_offset);
        return;
    }
    err = ecram_portio_write(ec_portio, ecram_offset, value);
    if (err)
        pr_info("Error writing EC RAM to 0x%x: Read-Only.\n",
            ecram_offset);
}

/* =============================== */
/* Reads from EC  */
/* ===============================  */

static u16 read_ec_id(struct ecram_portio* ec_portio, const struct model_config* model) {
    u8 id1 = ec_read(ec_portio, model->ec_register_offset.ECHIPID1);
    u8 id2 = ec_read(ec_portio, model->ec_register_offset.ECHIPID2);

    return (id1 << 8) + id2;
}

static u16 read_ec_version(struct ecram_portio* ec_portio, const struct model_config* model) {
    u8 vers = ec_read(ec_portio, model->ec_register_offset.ECHIPVER);
    u8 debug = ec_read(ec_portio, model->ec_register_offset.ECDEBUG);

    return (vers << 8) + debug;
}

struct iah7_private {
    struct platform_device* pdev;
    struct mutex mutex;
    bool loaded;
    struct model_config* config;
    struct ecram_portio ecram_portio;
    struct device* hwmon_dev;
};

struct iah7_private _priv;
struct iah7_private* iah7_shared;
static DEFINE_MUTEX(iah7_shared_mutex);

static int iah7_shared_init(struct iah7_private* priv) {
    int ret;

    mutex_lock(&iah7_shared_mutex);

    if (!iah7_shared) {
        iah7_shared = priv;
        mutex_init(&iah7_shared->mutex);
        ret = 0;
    } else {
        pr_warn("multiple platform devices\n");
        ret = -EINVAL;
    }

    priv->loaded = true;
    mutex_unlock(&iah7_shared_mutex);
    return ret;
}

static void iah7_shared_exit(struct iah7_private* priv) {
    mutex_lock(&iah7_shared_mutex);
    if (iah7_shared == priv) {
        iah7_shared = NULL;
    }
    mutex_unlock(&iah7_shared_mutex);
}

static ssize_t read_fanspeed(struct iah7_private* priv, int fan_id, int* speed_rpm) {
    unsigned long res;
    if (fan_id == 0) {
        res = ec_read(&priv->ecram_portio, priv->config->ec_register_offset.EXT_FAN1_RPM_LSB)
              + (((int) ec_read(&priv->ecram_portio, priv->config->ec_register_offset.EXT_FAN1_RPM_MSB)) << 8);
    } else if (fan_id == 1) {
        res = ec_read(&priv->ecram_portio, priv->config->ec_register_offset.EXT_FAN2_RPM_LSB)
              + (((int) ec_read(&priv->ecram_portio, priv->config->ec_register_offset.EXT_FAN2_RPM_MSB)) << 8);
    } else {
        return -EEXIST;
    }
    *speed_rpm = res;
    return 0;
}

enum SENSOR_ATTR {
    SENSOR_FAN1_RPM_ID = 1,
    SENSOR_FAN2_RPM_ID = 2,
};

static ssize_t sensor_show(struct device* dev, struct device_attribute* devattr, char* buf) {
    struct iah7_private* priv = dev_get_drvdata(dev);
    int sensor_id = (to_sensor_dev_attr(devattr))->index;
    int outval;
    int err = -EIO;


    switch (sensor_id) {
        case SENSOR_FAN1_RPM_ID:
            err = read_fanspeed(priv, 0, &outval);
            break;
        case SENSOR_FAN2_RPM_ID:
            err = read_fanspeed(priv, 1, &outval);
            break;
        default:
            dev_warn(dev, "sensor_show: unknown sensor id %d\n", sensor_id);
            err = -EOPNOTSUPP;
    }
    if (err) {
        return err;
    }
    return sprintf(buf, "%d\n", outval);
}

// static ssize_t sensor_label_show(struct device* dev, struct device_attribute* attr, char* buf) {
//     int sensor_id = (to_sensor_dev_attr(attr))->index;
//     const char *label;
//
//
//     switch (sensor_id) {
//         case SENSOR_FAN1_RPM_ID:
//             label = "Fan 1\n";
//             break;
//         case SENSOR_FAN2_RPM_ID:
//             label = "Fan 2\n";
//             break;
//         default:
//             return -EOPNOTSUPP;
//     }
//
//     return sprintf(buf, label);
// }

static SENSOR_DEVICE_ATTR_RO(fan1_input, sensor, SENSOR_FAN1_RPM_ID);
static SENSOR_DEVICE_ATTR_RO(fan2_input, sensor, SENSOR_FAN2_RPM_ID);
// static SENSOR_DEVICE_ATTR_RO(fan1_label, sensor_label, SENSOR_FAN1_RPM_ID);
// static SENSOR_DEVICE_ATTR_RO(fan2_lable, sensor_label, SENSOR_FAN2_RPM_ID);

static struct attribute* iah7_hwmon_sensor_attrs[] = {
    &sensor_dev_attr_fan1_input.dev_attr.attr,
    &sensor_dev_attr_fan2_input.dev_attr.attr,
    // &sensor_dev_attr_fan1_label.dev_attr.attr,
    // &sensor_dev_attr_fan2_lable.dev_attr.attr,
    NULL
};

static const struct attribute_group iah7_hwmon_sensor_group = {
    .attrs = iah7_hwmon_sensor_attrs,
    .is_visible = NULL,
};

static const struct attribute_group* iah7_hwmon_groups[] = {
    &iah7_hwmon_sensor_group, NULL
};

static int iah7_hwmon_init(void) {
    struct iah7_private* priv = iah7_shared;
    struct device* hwmon_dev = hwmon_device_register_with_groups(
        &priv->pdev->dev, "iah7_hwmon", priv, iah7_hwmon_groups
    );
    if (IS_ERR_OR_NULL(hwmon_dev)) {
        dev_err(&priv->pdev->dev, "hwmon_device_register_with_groups failed\n");
        return PTR_ERR(hwmon_dev);
    }
    dev_set_drvdata(hwmon_dev, priv);
    priv->hwmon_dev = hwmon_dev;
    return 0;
}

static void iah7_hwmon_exit(void) {
    struct iah7_private* priv = iah7_shared;
    if (priv->hwmon_dev) {
        hwmon_device_unregister(priv->hwmon_dev);
        priv->hwmon_dev = NULL;
    }
}

static int iah7_device_prob(struct platform_device* pdev) {
    struct iah7_private* priv;
    const struct dmi_system_id* dmi_sys;
    int err;
    bool do_laod;
    u16 ecid;
    dev_info(&pdev->dev, "lenovo_iah7 platform device prob");

    dev_info(&pdev->dev,
             "DMI_SYS_VENDOR: %s, DMI_PRODUCT_NAME: %s, DMI_BIOS_VERSION: %s",
             dmi_get_system_info(DMI_SYS_VENDOR), dmi_get_system_info(DMI_PRODUCT_NAME), dmi_get_system_info(DMI_BIOS_VERSION));

    priv = &_priv;
    priv->pdev = pdev;
    err = iah7_shared_init(priv);
    if (err) {
        dev_err(&pdev->dev, "lenovo_iah7 load err\n");
        goto err_iah7_shared_init;
    }
    dev_set_drvdata(&pdev->dev, priv);

    dmi_sys = dmi_first_match(iah7_dmi_ids);
    do_laod = force || dmi_sys != NULL;

    if (!do_laod) {
        dev_info(&pdev->dev, "lenovo_iah7 not matched\n");
        err = -ENODEV;
        goto err_module_mismatch;
    }

    if (force) {
        dev_warn(&pdev->dev, "lenovo_iah7 force loading\n");
        dmi_sys = &iah7_dmi_ids[0];
    }
    priv->config = dmi_sys->driver_data;
    dev_info(&pdev->dev, "loaded for device: %s", dmi_sys->ident);

    err = ecram_portio_init(&priv->ecram_portio);
    if (err) {
        dev_err(&pdev->dev, "ecram_portio_init failed\n");
        goto err_ecram_portio_init;
    }
    ecid = read_ec_id(&priv->ecram_portio, priv->config);
    dev_info(&pdev->dev, "ECID: 0x%x", ecid);

    dev_info(&pdev->dev, "create hwmon interface");
    err = iah7_hwmon_init();
    if (err) {
        dev_err(&pdev->dev, "hwmon_init failed\n");
        goto err_hwmon_init;
    }

    dev_info(&pdev->dev, "lenovo_iah7 loaded");
    return 0;
err_hwmon_init:
    iah7_hwmon_exit();
err_ecram_portio_init:
    ecram_portio_exit(&priv->ecram_portio);
err_iah7_shared_init:
    iah7_shared_exit(priv);
err_module_mismatch:
    return err;
}

static void iah7_device_remove(struct platform_device* pdev) {
    dev_info(&pdev->dev, "lenovo_iah7 platform device remove");
    struct iah7_private* priv = dev_get_drvdata(&pdev->dev);
    mutex_lock(&iah7_shared_mutex);
    priv->loaded = false;
    mutex_unlock(&iah7_shared_mutex);

    iah7_hwmon_exit();
    ecram_portio_exit(&priv->ecram_portio);
    iah7_shared_exit(priv);
    dev_info(&pdev->dev, "lenovo_iah7 unload");
}

static struct platform_driver iah7_driver = {
    .probe = iah7_device_prob,
    .remove = iah7_device_remove,
    .driver = {
        .name = "iah7"
    }
};

static int __init iah7_init(void) {
    int err;
    static struct platform_device* iah7_pdev;
    pr_info("loading lenovo_iah7\n");
    err = platform_driver_register(&iah7_driver);
    if (err) {
        pr_err("platform_driver_register failed\n");
        return err;
    }
    iah7_pdev = platform_device_register_simple("iah7", -1, NULL, 0);
    if (IS_ERR(iah7_pdev)) {
        pr_err("platform_device_register_simple failed\n");
        platform_driver_unregister(&iah7_driver);
        return PTR_ERR(iah7_pdev);
    }
    return 0;
}

static void __exit iah7_exit(void) {
    platform_device_unregister(_priv.pdev);
    platform_driver_unregister(&iah7_driver);
    pr_info("lenovo_iah7 exit");
}

module_init(iah7_init)
module_exit(iah7_exit)

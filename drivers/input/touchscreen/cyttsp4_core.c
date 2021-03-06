/*
 * Core Source for:
 * Cypress TrueTouch(TM) Standard Product (TTSP) touchscreen drivers.
 * For use with Cypress Gen4 and Solo parts.
 * Supported parts include:
 * CY8CTMA884/616
 * CY8CTMA4XX
 *
 * Copyright (C) 2009-2012 Cypress Semiconductor, Inc.
 * Copyright (C) 2011 Motorola Mobility, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2, and only version 2, as published by the
 * Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * Contact Cypress Semiconductor at www.cypress.com <kev@cypress.com>
 *
 */
#include "cyttsp4_core.h"

#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/slab.h>
#include <linux/input/touch_platform.h>
#include <linux/firmware.h>	/* This enables firmware class loader code */
#include <linux/string.h>

#include <mach/bowser_idme_init.h>
#include <linux/input/cyttsp4_params.h>

/* platform address lookup offsets */
#define CY_TCH_ADDR_OFS		0
#define CY_LDR_ADDR_OFS		1

/* helpers */
#define GET_NUM_TOUCHES(x)          ((x) & 0x1F)
#define IS_LARGE_AREA(x)            ((x) & 0x20)
#define IS_BAD_PKT(x)               ((x) & 0x20)
#define GET_HSTMODE(reg)            ((reg & 0x70) >> 4)
#define IS_BOOTLOADERMODE(reg)      (reg & 0x01)
#define GET_RECORD_COUNT(reg)       (reg & 0xC0)

/* maximum number of concurrent tracks */
#define CY_NUM_TCH_ID               10
/* maximum number of track IDs */
#define CY_NUM_TRK_ID               16
/* maximum number of command data bytes */
#define CY_NUM_DAT                  6
/* maximum number of config block read data */
#define CY_NUM_CONFIG_BYTES        128

#define CY_NUM_CAT_DATA            252

#define CY_REG_BASE                 0x00
#define CY_DELAY_DFLT               20		/* ms */
#define CY_DELAY_MAX                (500/CY_DELAY_DFLT)	/* half second */
#define CY_HALF_SEC_TMO_MS          500		/* half second in msecs */
#define CY_ONE_SEC_TMO_MS           1000		/* half second in msecs */
#define CY_TEN_SEC_TMO_MS           10000	/* ten seconds in msecs */
#define CY_HANDSHAKE_BIT            0x80
#define CY_WAKE_DFLT                99	/* causes wake strobe on INT line
					 * in sample board configuration
					 * platform data->hw_recov() function
					 */
/* power mode select bits */
#define CY_SOFT_RESET_MODE          0x01
#define CY_DEEP_SLEEP_MODE          0x02
#define CY_LOW_POWER_MODE           0x04
/* device mode bits */
#define CY_MODE_CHANGE              0x08 /* rd/wr hst_mode */
#define CY_OPERATE_MODE             0x00 /* rd/wr hst_mode */
#define CY_SYSINFO_MODE             0x10 /* rd/wr hst_mode */
#define CY_CONFIG_MODE              0x20 /* rd/wr hst_mode */
#define CY_BL_MODE                  0x01 /* wr hst mode == soft reset
					  * was 0x10 to rep_stat for LTS
					  */
#define CY_CMD_RDY_BIT              0x40

#define CY_REG_OP_START             0
#define CY_REG_SI_START             0
#define CY_REG_OP_END               0x20
#define CY_REG_SI_END               0x20


/* register field lengths */
#define CY_NUM_REVCTRL              8
#define CY_NUM_MFGID                8
#define CY_NUM_TCHREC               10
#define CY_NUM_DDATA                32
#define CY_NUM_MDATA                64
#define CY_TMA884_MAX_BYTES         255 /*
					  * max reg access for TMA884
					  * in config mode
					  */
#define CY_TMA400_MAX_BYTES         512 /*
					  * max reg access for TMA400
					  * in config mode
					  */

/* touch event id codes */
#define CY_GET_EVENTID(reg)         ((reg & 0x60) >> 5)
#define CY_GET_TRACKID(reg)         (reg & 0x1F)
#define CY_NOMOVE                   0
#define CY_TOUCHDOWN                1
#define CY_MOVE                     2
#define CY_LIFTOFF                  3

#define CY_CFG_BLK_SIZE             126
#define CY_EBID_ROW_SIZE_DFLT       128

#define CY_BL_VERS_SIZE             12
#define CY_NUM_TMA400_TT_CFG_BLK    51 /* Rev84 mapping */

#if defined(CY_USE_FORCE_LOAD) || defined(CONFIG_TOUCHSCREEN_DEBUG)
#define CY_BL_FW_NAME_SIZE          NAME_MAX
#endif

#ifdef CONFIG_TOUCHSCREEN_DEBUG
#define CY_BL_TXT_FW_IMG_SIZE       128261
#define CY_BL_BIN_FW_IMG_SIZE       128261
#define CY_NUM_PKG_PKT              4
#define CY_NUM_PKT_DATA             32
#define CY_MAX_PKG_DATA             (CY_NUM_PKG_PKT * CY_NUM_PKT_DATA)
#define CY_MAX_IC_BUF               256
#endif /* --CONFIG_TOUCHSCREEN_DEBUG */

#ifdef CY_USE_REG_ACCESS
#define CY_RW_REGID_MAX             0xFFFF
#define CY_RW_REG_DATA_MAX          0xFF
#endif

#define CY_NONE 0x00
#define CY_CHARGER_ONLY 0x01
#define CY_HDMI_ONLY 0x02
#define CY_CHARGER_HDMI 0x03

/* defines for indices into parameter list */
#define CY_CHARGER_HDMI_INDEX 0
#define CY_AFH_OPMODE_INDEX 55

/* abs settings */
#define CY_IGNORE_VALUE             0xFFFF
/* abs signal capabilities offsets in the frameworks array */
enum cyttsp4_sig_caps {
	CY_SIGNAL_OST   = 0,
	CY_MIN_OST      = 1,
	CY_MAX_OST      = 2,
	CY_FUZZ_OST     = 3,
	CY_FLAT_OST     = 4,
	CY_NUM_ABS_SET	/* number of signal capability fields */
};

/* abs axis signal offsets in the framworks array  */
enum cyttsp4_sig_ost {
	CY_ABS_X_OST    = 0,
	CY_ABS_Y_OST    = 1,
	CY_ABS_P_OST    = 2,
	CY_ABS_W_OST    = 3,
	CY_ABS_ID_OST   = 4,
	CY_ABS_MAJ_OST	= 5,
	CY_ABS_MIN_OST	= 6,
	CY_ABS_OR_OST	= 7,
	CY_NUM_ABS_OST	/* number of abs signals */
};

/* touch record system information offset masks and shifts */
#define CY_BYTE_OFS_MASK            0x1F
#define CY_BOFS_MASK                0xE0
#define CY_BOFS_SHIFT               5

enum cyttsp4_driver_state {
	CY_IDLE_STATE,		/* IC cannot be reached */
	CY_READY_STATE,		/* pre-operational; ready to go to ACTIVE */
	CY_ACTIVE_STATE,	/* app is running, IC is scanning */
	CY_SLEEP_STATE,		/* app is running, IC is idle */
	CY_BL_STATE,		/* bootloader is running */
	CY_SYSINFO_STATE,	/* switching to sysinfo mode */
	CY_CMD_STATE,		/* command initiation mode */
	CY_EXIT_BL_STATE,	/* sync bl heartbeat to app ready int */
	CY_TRANSFER_STATE,	/* changing states */
	CY_OPCMD_STATE,     /* new state to allow for run time command processing */
	CY_INVALID_STATE	/* always last in the list */
};

static const char * const cyttsp4_driver_state_string[] = {
	/* Order must match enum cyttsp4_driver_state above */
	"IDLE",
	"READY",
	"ACTIVE",
	"SLEEP",
	"BOOTLOADER",
	"SYSINFO",
	"CMD",
	"EXIT_BL",
	"TRANSFER",
	"OPCMD",
	"INVALID"
};

enum cyttsp4_controller_mode {
	CY_MODE_BOOTLOADER,
	CY_MODE_SYSINFO,
	CY_MODE_OPERATIONAL,
	CY_MODE_CONFIG,
	CY_MODE_NUM
};

enum cyttsp4_ic_grpnum {
	CY_IC_GRPNUM_RESERVED = 0,
	CY_IC_GRPNUM_CMD_REGS,
	CY_IC_GRPNUM_TCH_REP,
	CY_IC_GRPNUM_DATA_REC,
	CY_IC_GRPNUM_TEST_REC,
	CY_IC_GRPNUM_PCFG_REC,
	CY_IC_GRPNUM_TCH_PARM_VAL,
	CY_IC_GRPNUM_TCH_PARM_SIZ,
	CY_IC_GRPNUM_RESERVED1,
	CY_IC_GRPNUM_RESERVED2,
	CY_IC_GRPNUM_OPCFG_REC,
	CY_IC_GRPNUM_DDATA_REC,
	CY_IC_GRPNUM_MDATA_REC,
	CY_IC_GRPNUM_TEST_REGS,
	CY_IC_GRPNUM_BTN_KEYS,
	CY_IC_GRPNUM_NUM
};

enum cyttsp4_ic_op_mode_commands {
	CY_GET_PARAM_CMD = 0x02,
	CY_SET_PARAM_CMD = 0x03,
	CY_GET_CFG_BLK_CRC = 0x05,
	CY_GET_CHRGHDMI_BIT = 0x27,
	CY_SET_CHRGHDMI_BIT = 0x28,
};

enum cyttsp4_ic_config_mode_commands {
	CY_GET_EBID_ROW_SIZE = 0x02,
	CY_READ_EBID_DATA = 0x03,
	CY_WRITE_EBID_DATA = 0x04,
	CY_VERIFY_EBID_CRC = 0x11,
};

#ifdef CY_USE_TMA884
enum cyttsp4_ic_ebid {
	CY_TCH_PARM_EBID = 0x00,
	CY_DDATA_EBID = 0x05,
	CY_MDATA_EBID = 0x06,
};
#endif /* --CY_USE_TMA884 */

enum cyttsp4_flags {
	CY_FLAG_NONE = 0x00,
	CY_FLAG_HOVER = 0x04,
#ifdef CY_USE_DEBUG_TOOLS
	CY_FLAG_FLIP = 0x08,
	CY_FLAG_INV_X = 0x10,
	CY_FLAG_INV_Y = 0x20,
#endif /* --CY_USE_DEBUG_TOOLS */
};

enum cyttsp4_event_id {
	CY_EV_NO_EVENT = 0,
	CY_EV_TOUCHDOWN = 1,
	CY_EV_MOVE = 2,		/* significant displacement (> act dist) */
	CY_EV_LIFTOFF = 3,	/* record reports last position */
};

enum cyttsp4_object_id {
	CY_OBJ_STANDARD_FINGER = 0,
	CY_OBJ_LARGE_OBJECT = 1,
	CY_OBJ_STYLUS = 2,
	CY_OBJ_HOVER = 3,
};

enum cyttsp4_test_cmd {
	CY_TEST_CMD_NULL = 0,
};

/* test mode NULL command driver codes; D */
enum cyttsp4_null_test_cmd_code {
	CY_NULL_CMD_NULL = 0,
	CY_NULL_CMD_MODE,
	CY_NULL_CMD_STATUS_SIZE,
	CY_NULL_CMD_HANDSHAKE,
};

enum cyttsp_test_mode {
	CY_TEST_MODE_NORMAL_OP = 0,	/* Send touch data to OS; normal op */
	CY_TEST_MODE_CAT,		/* Configuration and Test */
	CY_TEST_MODE_CLOSED_UNIT,	/* Send scan data to sysfs */
};

struct cyttsp4_test_mode {
	int cur_mode;
	int cur_cmd;
	size_t cur_status_size;
};

/* GEN4/SOLO Operational interface definitions */
enum cyttsp4_tch_abs {	/* for ordering within the extracted touch data array */
	CY_TCH_X = 0,	/* X */
	CY_TCH_Y,	/* Y */
	CY_TCH_P,	/* P (Z) */
	CY_TCH_T,	/* TOUCH ID */
	CY_TCH_E,	/* EVENT ID */
	CY_TCH_O,	/* OBJECT ID */
	CY_TCH_W,	/* SIZE (SOLO - Corresponds to TOUCH_MAJOR) */
	CY_TCH_NUM_ABS
};
static const char * const cyttsp4_tch_abs_string[] = {
	/* Order must match enum cyttsp4_tch_descriptor above */
	"X",
	"Y",
	"P",
	"T",
	"E",
	"O",
	"W",
	"INVALID"
};

#ifdef CY_USE_TMA884
#define CY_NUM_NEW_TCH_FIELDS	0
#endif /* --CY_USE_TMA884 */

#define CY_NUM_OLD_TCH_FIELDS	(CY_TCH_NUM_ABS - CY_NUM_NEW_TCH_FIELDS)

struct cyttsp4_touch {
	int abs[CY_TCH_NUM_ABS];
};

struct cyttsp4_catdata {
	u8 hst_mode;
	u8 reserved;
	u8 cmd;
	u8 data[CY_NUM_CAT_DATA];
} __packed;

/* TTSP System Information interface definitions */
struct cyttsp4_cydata {
	u8 ttpidh;
	u8 ttpidl;
	u8 fw_ver_major;
	u8 fw_ver_minor;
	u8 revctrl[CY_NUM_REVCTRL];
	u8 blver_major;
	u8 blver_minor;
	u8 jtag_si_id3;
	u8 jtag_si_id2;
	u8 jtag_si_id1;
	u8 jtag_si_id0;
	u8 mfgid_sz;
	u8 mfg_id[CY_NUM_MFGID];
	u8 cyito_idh;
	u8 cyito_idl;
	u8 cyito_verh;
	u8 cyito_verl;
	u8 ttsp_ver_major;
	u8 ttsp_ver_minor;
	u8 device_info;
} __packed;

struct cyttsp4_test {
	u8 post_codeh;
	u8 post_codel;
} __packed;

struct cyttsp4_pcfg {
	u8 electrodes_x;
	u8 electrodes_y;
	u8 len_xh;
	u8 len_xl;
	u8 len_yh;
	u8 len_yl;
	u8 axis_xh;
	u8 axis_xl;
	u8 axis_yh;
	u8 axis_yl;
	u8 max_zh;
	u8 max_zl;
} __packed;

struct cyttsp4_tch_rec_params {
	u8 loc;
	u8 size;
} __packed;

struct cyttsp4_opcfg {
	u8 cmd_ofs;
	u8 rep_ofs;
	u8 rep_szh;
	u8 rep_szl;
	u8 num_btns;
	u8 tt_stat_ofs;
	u8 obj_cfg0;
	u8 max_tchs;
	u8 tch_rec_siz;
	struct cyttsp4_tch_rec_params tch_rec_old[CY_NUM_OLD_TCH_FIELDS];
	u8 btn_rec_siz;	/* btn record size (in bytes) */
	u8 btn_diff_ofs;/* btn data loc ,diff counts, (Op-Mode byte ofs) */
	u8 btn_diff_siz;/* btn size of diff counts (in bits) */
} __packed;

struct cyttsp4_sysinfo_data {
	u8 hst_mode;
	u8 reserved;
	u8 map_szh;
	u8 map_szl;
	u8 cydata_ofsh;
	u8 cydata_ofsl;
	u8 test_ofsh;
	u8 test_ofsl;
	u8 pcfg_ofsh;
	u8 pcfg_ofsl;
	u8 opcfg_ofsh;
	u8 opcfg_ofsl;
	u8 ddata_ofsh;
	u8 ddata_ofsl;
	u8 mdata_ofsh;
	u8 mdata_ofsl;
} __packed;

struct cyttsp4_sysinfo_ptr {
	struct cyttsp4_cydata *cydata;
	struct cyttsp4_test *test;
	struct cyttsp4_pcfg *pcfg;
	struct cyttsp4_opcfg *opcfg;
	struct cyttsp4_ddata *ddata;
	struct cyttsp4_mdata *mdata;
} __packed;

struct cyttsp4_tch_abs_params {
	size_t ofs;	/* abs byte offset */
	size_t size;	/* size in bits */
	size_t max;	/* max value */
	size_t bofs;	/* bit offset */
};

struct cyttsp4_sysinfo_ofs {
	size_t cmd_ofs;
	size_t rep_ofs;
	size_t rep_sz;
	size_t num_btns;
	size_t num_btn_regs;	/* ceil(num_btns/4) */
	size_t tt_stat_ofs;
	size_t tch_rec_siz;
	size_t obj_cfg0;
	size_t max_tchs;
	size_t mode_size;
	size_t data_size;
	size_t map_sz;
	size_t cydata_ofs;
	size_t test_ofs;
	size_t pcfg_ofs;
	size_t opcfg_ofs;
	size_t ddata_ofs;
	size_t mdata_ofs;
	size_t cydata_size;
	size_t test_size;
	size_t pcfg_size;
	size_t opcfg_size;
	size_t ddata_size;
	size_t mdata_size;
	size_t btn_keys_size;
	struct cyttsp4_tch_abs_params tch_abs[CY_TCH_NUM_ABS];
	size_t btn_rec_siz;	/* btn record size (in bytes) */
	size_t btn_diff_ofs;/* btn data loc ,diff counts, (Op-Mode byte ofs) */
	size_t btn_diff_siz;/* btn size of diff counts (in bits) */
};

/* button to keycode support */
#define CY_NUM_BTN_PER_REG	4
#define CY_NUM_BTN_EVENT_ID	4
#define CY_BITS_PER_BTN		2

enum cyttsp4_btn_state {
	CY_BTN_RELEASED = 0,
	CY_BTN_PRESSED = 1,
	CY_BTN_NUM_STATE
};

struct cyttsp4_btn {
	bool enabled;
	int state;	/* CY_BTN_PRESSED, CY_BTN_RELEASED */
	int key_code;
};

#ifdef CONFIG_TOUCHSCREEN_DEBUG
struct cyttsp4_dbg_pkg {
	bool ready;
	int cnt;
	u8 data[CY_MAX_PKG_DATA];
	};
#endif /* --CONFIG_TOUCHSCREEN_DEBUG */

/* driver context structure definitions */

struct cyttsp4 {
	struct device *dev;
	int irq;
	struct input_dev *input;
	struct mutex data_lock;		/* prevent concurrent accesses */
	struct workqueue_struct		*cyttsp4_wq;
	struct work_struct		cyttsp4_resume_startup_work;
	char phys[32];
	const struct bus_type *bus_type;
	struct touch_platform_data *platform_data;
	u8 *xy_mode;			/* operational mode and status regs */
	u8 *xy_data;			/* operational touch regs */
	u8 *xy_data_touch1;		/* includes 1-byte for tt_stat */
	u8 *btn_rec_data;		/* button diff count data */
	struct cyttsp4_bus_ops *bus_ops;
	struct cyttsp4_sysinfo_data sysinfo_data;
	struct cyttsp4_sysinfo_ptr sysinfo_ptr;
	struct cyttsp4_sysinfo_ofs si_ofs;
	struct cyttsp4_btn *btn;
	struct cyttsp4_test_mode test;
	struct completion int_running;
	struct completion si_int_running;
	struct completion ready_int_running;
	enum cyttsp4_driver_state driver_state;
	enum cyttsp4_controller_mode current_mode;
	bool irq_enabled;
	bool powered; /* protect against multiple open */
	bool was_suspended;
	bool switch_flag;
	bool soft_reset_asserted;
	u16 flags;
	u8 charger_hdmi;
	bool charger_hdmi_update_pending;
	size_t max_config_bytes;
	size_t ebid_row_size;
	int num_prv_tch;
	u8 prev_record_count;

#ifdef CONFIG_HAS_EARLYSUSPEND
	struct early_suspend early_suspend;
#endif
#ifdef CY_USE_WATCHDOG
	struct work_struct work;
	struct timer_list timer;
#endif
#if defined(CY_USE_FORCE_LOAD) || defined(CONFIG_TOUCHSCREEN_DEBUG)
	bool waiting_for_fw;
	char *fwname;
#endif
#ifdef CONFIG_TOUCHSCREEN_DEBUG
	u8 *pr_buf;
	bool debug_upgrade;
	int ic_grpnum;
	int ic_grpoffset;
	bool ic_grptest;
#endif /* --CONFIG_TOUCHSCREEN_DEBUG */

#ifdef CY_USE_REG_ACCESS
	size_t rw_regid;
#endif
#ifdef CONFIG_TOUCHSCREEN_DEBUG_ENABLE_ENTRY
	bool debug_enable;
#endif
	bool low_power_enable;
	bool sysfs_files_created;
	bool suspend_blocked;
	bool suspend_in_prog;
	bool resume_in_prog;
	struct mutex suspend_lock;		/* suspend/resume lock */
};

#if defined(CY_AUTO_LOAD_FW) || \
	defined(CY_USE_FORCE_LOAD) || \
	defined(CONFIG_TOUCHSCREEN_DEBUG)
static int _cyttsp4_load_app(struct cyttsp4 *ts, const u8 *fw, int fw_size);
#endif /* CY_AUTO_LOAD_FW || CY_USE_FORCE_LOAD || CONFIG_TOUCHSCREEN_DEBUG */
static int _cyttsp4_ldr_exit(struct cyttsp4 *ts);
static int _cyttsp4_startup(struct cyttsp4 *ts);
static int _cyttsp4_get_ic_crc(struct cyttsp4 *ts,
	enum cyttsp4_ic_ebid ebid, u8 *crc_h, u8 *crc_l);
static irqreturn_t cyttsp4_irq(int irq, void *handle);
static int _cyttsp4_set_mode(struct cyttsp4 *ts, u8 new_mode);
#ifdef CY_USE_TMA884
static int _cyttsp4_calc_data_crc(struct cyttsp4 *ts,
	size_t ndata, u8 *pdata, u8 *crc_h, u8 *crc_l, const char *name);
#endif /* --CY_USE_TMA884 */
int write_charger_hdmi_config(struct cyttsp4 *ts, u8 value);


#ifdef CONFIG_MACH_OMAP4_BOWSER_SUBTYPE_JEM_FTM
unsigned char ftm_test_signal_data[1000] = {0};
int ftm_test_total_points;
#endif


static void _cyttsp4_pr_state(struct cyttsp4 *ts)
{

	dev_dbg(ts->dev,
		 "%s: %s\n", __func__,
		 ts->driver_state < CY_INVALID_STATE ?
		 cyttsp4_driver_state_string[ts->driver_state] :
		 "INVALID");
}

static void _cyttsp4_pr_buf(struct cyttsp4 *ts, u8 *dptr, int size,
	const char *data_name)
{
#ifdef CONFIG_TOUCHSCREEN_DEBUG
	int i = 0;
	int max = (CY_MAX_PRBUF_SIZE - 1) - sizeof(CY_PR_TRUNCATED);

	if (ts == NULL)
		dev_err(ts->dev,
				"%s: ts=%p\n", __func__, ts);
	else if (ts->pr_buf == NULL)
		dev_err(ts->dev,
				"%s: ts->pr_buf=%p\n", __func__, ts->pr_buf);
	else if (ts->bus_ops->tsdebug >= CY_DBG_LVL_2) {
		ts->pr_buf[0] = 0;
		for (i = 0; i < size && i < max; i++)
				sprintf(ts->pr_buf, "%s %02X", ts->pr_buf, dptr[i]);
		dev_info(ts->dev,
				"%s:  %s[0..%d]=%s%s\n", __func__, data_name, size-1,
					ts->pr_buf, size <= max ? "" : CY_PR_TRUNCATED);
	}
#endif /* --CONFIG_TOUCHSCREEN_DEBUG */
	return;
}

static int _cyttsp4_read_block_data(struct cyttsp4 *ts, u16 command,
	size_t length, void *buf, int i2c_addr, bool use_subaddr)
{
	int retval = 0;
	int tries = 0;

#ifdef CONFIG_TOUCHSCREEN_DEBUG_ENABLE_ENTRY
	/* if debugmode is set, the driver won't send command to chip */
	if (ts->debug_enable == true)
		return retval;
#endif
	if ((buf == NULL) || (length == 0)) {
		dev_err(ts->dev,
			"%s: pointer or length error"
			" buf=%p length=%d\n", __func__, buf, length);
		retval = -EINVAL;
	} else {
		for (tries = 0, retval = -1;
			tries < CY_NUM_RETRY && (retval < 0);
			tries++) {
			retval = ts->bus_ops->read(ts->bus_ops, command,
				length, buf, i2c_addr, use_subaddr);
			if (retval < 0) {
				msleep(CY_DELAY_DFLT);
				/*
				 * TODO: remove the extra sleep delay when
				 * the loader exit sequence is streamlined
				  */
				msleep(150);
			}
		}

		if (retval < 0) {
			dev_err(ts->dev,
			"%s: bus read block data fail (ret=%d)\n",
				__func__, retval);
		}
	}

	return retval;
}

static int _cyttsp4_write_block_data(struct cyttsp4 *ts, u16 command,
	size_t length, const void *buf, int i2c_addr, bool use_subaddr)
{
	int retval = 0;
	int tries = 0;

#ifdef CONFIG_TOUCHSCREEN_DEBUG_ENABLE_ENTRY
	/* if debugmode is set, the driver won't send command to chip */
	if (ts->debug_enable == true)
		return retval;
#endif
	if ((buf == NULL) || (length == 0)) {
		dev_err(ts->dev,
			"%s: pointer or length error"
			" buf=%p length=%d\n", __func__, buf, length);
		retval = -EINVAL;
	} else {
		for (tries = 0, retval = -1;
			tries < CY_NUM_RETRY && (retval < 0);
			tries++) {
			retval = ts->bus_ops->write(ts->bus_ops, command,
				length, buf, i2c_addr, use_subaddr);
			if (retval < 0)
				msleep(CY_DELAY_DFLT);
		}

		if (retval < 0) {
			dev_err(ts->dev,
			"%s: bus write block data fail (ret=%d)\n",
				__func__, retval);
		}
	}

	return retval;
}


static int _cyttsp4_wait_int_no_init(struct cyttsp4 *ts,
	unsigned long timeout_ms)
{
	unsigned long uretval;
	int retval = 0;

	mutex_unlock(&ts->data_lock);
	uretval = wait_for_completion_interruptible_timeout(
		&ts->int_running, msecs_to_jiffies(timeout_ms));
	mutex_lock(&ts->data_lock);
	if (uretval == 0) {
		dev_err(ts->dev,
			"%s: timeout waiting for interrupt\n",
			__func__);
		retval = -ETIMEDOUT;
	}

	return retval;
}

static int _cyttsp4_wait_int(struct cyttsp4 *ts, unsigned long timeout_ms)
{
	int retval = 0;

	INIT_COMPLETION(ts->int_running);
	retval = _cyttsp4_wait_int_no_init(ts, timeout_ms);
	if (retval < 0) {
		dev_err(ts->dev,
			"%s: timeout waiting for interrupt\n",
			__func__);
	}

	return retval;
}

static int _cyttsp4_wait_si_int(struct cyttsp4 *ts, unsigned long timeout_ms)
{
	unsigned long uretval;
	int retval = 0;

	mutex_unlock(&ts->data_lock);
	uretval = wait_for_completion_interruptible_timeout(
		&ts->si_int_running, msecs_to_jiffies(timeout_ms));
	mutex_lock(&ts->data_lock);
	if (uretval == 0) {
		dev_err(ts->dev,
			"%s: timeout waiting for bootloader interrupt\n",
			__func__);
		retval = -ETIMEDOUT;
	}

	return retval;
}

static void _cyttsp4_queue_startup(struct cyttsp4 *ts, bool was_suspended)
{
	ts->was_suspended = was_suspended;
	queue_work(ts->cyttsp4_wq,
		&ts->cyttsp4_resume_startup_work);
	dev_info(ts->dev,
			"%s: startup queued\n", __func__);
}

#if defined(CY_AUTO_LOAD_TOUCH_PARAMS) || \
	defined(CY_AUTO_LOAD_DDATA) || defined(CY_AUTO_LOAD_MDATA) || \
	defined(CY_USE_DEV_DEBUG_TOOLS) || defined(CY_USE_TMA884)
static u16 _cyttsp4_calc_partial_crc(struct cyttsp4 *ts,
	u8 *pdata, size_t ndata, u16 crc)
{
	int i = 0;
	int j = 0;

	for (i = 0; i < ndata; i++) {
		crc ^= ((u16)pdata[i] << 8);

		for (j = 8; j > 0; --j) {
			if (crc & 0x8000)
				crc = (crc << 1) ^ 0x1021;
			else
				crc = crc << 1;
		}
	}

	return crc;
}

static void _cyttsp4_calc_crc(struct cyttsp4 *ts,
	u8 *pdata, size_t ndata, u8 *crc_h, u8 *crc_l)
{
	u16 crc = 0;

	if (pdata == NULL)
		dev_err(ts->dev,
			"%s: Null data ptr\n", __func__);
	else if (ndata == 0)
		dev_err(ts->dev,
			"%s: Num data is 0\n", __func__);
	else {
		/* Calculate CRC */
		crc = 0xFFFF;
		crc = _cyttsp4_calc_partial_crc(ts, pdata, ndata, crc);
		*crc_h = crc / 256;
		*crc_l = crc % 256;
	}
}
#endif /* --CY_AUTO_LOAD_TOUCH_PARAMS --CY_AUTO_LOAD_DDATA
	--CY_AUTO_LOAD_MDATA --CY_USE_DEV_DEBUG_TOOLS --CY_USE_TMA884 */

static bool _cyttsp4_chk_cmd_rdy(struct cyttsp4 *ts, u8 cmd)
{
	bool cond = !!(cmd & CY_CMD_RDY_BIT);
	dev_vdbg(ts->dev,
		"%s: cmd=%02X cond=%d\n", __func__, cmd, (int)cond);

	return cond;
}

static bool _cyttsp4_chk_mode_change(struct cyttsp4 *ts, u8 cmd)
{
	bool cond = !(cmd & CY_MODE_CHANGE);
	dev_vdbg(ts->dev,
		"%s: cmd=%02X cond=%d\n", __func__, cmd, (int)cond);

	return cond;
}

static void _cyttsp4_change_state(struct cyttsp4 *ts,
	enum cyttsp4_driver_state new_state)
{
	ts->driver_state = new_state;
	_cyttsp4_pr_state(ts);
}

static int _cyttsp4_put_cmd_wait(struct cyttsp4 *ts, u16 ofs,
	size_t cmd_len, const void *cmd_buf, unsigned long timeout_ms,
	bool (*cond)(struct cyttsp4 *, u8), u8 *retcmd,
	int i2c_addr, bool use_subaddr, enum cyttsp4_driver_state cmd_state)
{
	enum cyttsp4_driver_state tmp_state;
	unsigned long uretval = 0;
	u8 cmd = 0;
	int tries = 0;
	int retval = 0;

	/* unlock here to allow any pending irq to complete */
	tmp_state = ts->driver_state;
	if(cmd_state != CY_OPCMD_STATE)
		_cyttsp4_change_state(ts, CY_TRANSFER_STATE);

	mutex_unlock(&ts->data_lock);
	mutex_lock(&ts->data_lock);
	_cyttsp4_change_state(ts, cmd_state);
	INIT_COMPLETION(ts->int_running);
	mutex_unlock(&ts->data_lock);
	retval = _cyttsp4_write_block_data(ts, ofs, cmd_len,
		cmd_buf, i2c_addr, use_subaddr);
	if (retval < 0) {
		dev_err(ts->dev,
			"%s: Fail writing cmd buf r=%d\n",
			__func__, retval);
		mutex_lock(&ts->data_lock);
		goto _cyttsp4_put_cmd_wait_exit;
	}
_cyttsp4_put_cmd_wait_retry:
	uretval = wait_for_completion_interruptible_timeout(
		&ts->int_running, msecs_to_jiffies(timeout_ms));
	mutex_lock(&ts->data_lock);

	retval = _cyttsp4_read_block_data(ts, ofs,
		sizeof(cmd), &cmd, i2c_addr, use_subaddr);
	if (retval < 0) {
		dev_err(ts->dev,
			"%s: fail read cmd status  r=%d\n",
			__func__, retval);
	}
	if ((cond != NULL) && !cond(ts, cmd)) {
		if (uretval == 0) {
			dev_err(ts->dev,
			"%s: timeout waiting for cmd ready\n",
				__func__);
			retval = -ETIMEDOUT;
		} else {
			if (tries++ < 2) {
				INIT_COMPLETION(ts->int_running);
				mutex_unlock(&ts->data_lock);
				goto _cyttsp4_put_cmd_wait_retry;
			} else {
				dev_err(ts->dev,
			"%s: cmd not ready error"
					" cmd_stat=0x%02X\n",
					__func__, cmd);
				retval = -EIO;
			}
		}
	} else {
		/* got command ready */
		if (retcmd != NULL)
			*retcmd = cmd;
		retval = 0;
		dev_vdbg(ts->dev,
			"%s: got command ready; cmd=%02X retcmd=%p tries=%d\n",
			__func__, cmd, retcmd, tries);
	}

_cyttsp4_put_cmd_wait_exit:
	_cyttsp4_change_state(ts, tmp_state);
	return retval;
}

static int _cyttsp4_handshake(struct cyttsp4 *ts, u8 hst_mode)
{
	int retval = 0;
	u8 cmd = 0;

	cmd = hst_mode & CY_HANDSHAKE_BIT ?
		hst_mode & ~CY_HANDSHAKE_BIT :
		hst_mode | CY_HANDSHAKE_BIT;

	if(ts->low_power_enable == true)
		cmd |= CY_LOW_POWER_MODE;
	else
		cmd &= (~CY_LOW_POWER_MODE);

	retval = _cyttsp4_write_block_data(ts, CY_REG_BASE,
		sizeof(cmd), (u8 *)&cmd,
		ts->platform_data->addr[CY_TCH_ADDR_OFS], true);

	if (retval < 0) {
		dev_err(ts->dev,
			"%s: bus write fail on handshake (ret=%d)\n",
			__func__, retval);
	}

	return retval;
}

static int _cyttsp4_cmd_handshake(struct cyttsp4 *ts)
{
	u8 host_mode = 0;
	int retval = 0;

	retval = _cyttsp4_read_block_data(ts, CY_REG_BASE,
		sizeof(host_mode), &host_mode,
		ts->platform_data->addr[CY_TCH_ADDR_OFS], true);
	if (retval < 0) {
		dev_err(ts->dev,
			"%s: Fail read host mode r=%d\n",
			__func__, retval);
	} else {
		retval = _cyttsp4_handshake(ts, host_mode);
		if (retval < 0) {
			dev_err(ts->dev,
			"%s: Fail handshake r=%d\n",
				__func__, retval);
		}
	}

	return retval;
}


#ifdef CY_USE_TMA884
static int _cyttsp4_handshake_enable(struct cyttsp4 *ts)
{
	int retval = 0;
	u8 cmd_dat[CY_NUM_DAT + 1];	/* +1 for cmd byte */

	memset(cmd_dat, 0, sizeof(cmd_dat));
	cmd_dat[0] = 0x26;	/* handshake enable operational cmd */
	cmd_dat[1] = 0x03;	/* synchronous level handshake */
	retval = _cyttsp4_put_cmd_wait(ts, ts->si_ofs.cmd_ofs,
		sizeof(cmd_dat), cmd_dat, CY_HALF_SEC_TMO_MS,
		_cyttsp4_chk_cmd_rdy, NULL,
		ts->platform_data->addr[CY_TCH_ADDR_OFS], true, CY_CMD_STATE);
	if (retval < 0) {
		dev_err(ts->dev,
			"%s: Fail Enable Handshake command r=%d\n",
			__func__, retval);
		goto _cyttsp4_set_handshake_enable_exit;
	}

	retval = _cyttsp4_read_block_data(ts, ts->si_ofs.cmd_ofs,
		sizeof(cmd_dat), cmd_dat,
		ts->platform_data->addr[CY_TCH_ADDR_OFS], true);
	if (retval < 0) {
		dev_err(ts->dev,
			"%s: Fail read Enable Hanshake command statu_cyttsp4_handshakes"
			"r=%d\n", __func__, retval);
		goto _cyttsp4_set_handshake_enable_exit;
	}

	if (cmd_dat[6] != cmd_dat[1]) {
		dev_err(ts->dev,
			"%s: Fail enable handshake in device\n",
			__func__);
		/* return no error and let driver handshake anyway */
	}

	dev_vdbg(ts->dev,
		"%s: check cmd ready r=%d"
		" cmd[]=%02X %02X %02X %02X %02X %02X %02X\n",
		__func__, retval,
		cmd_dat[0], cmd_dat[1], cmd_dat[2], cmd_dat[3],
		cmd_dat[4], cmd_dat[5], cmd_dat[6]);

_cyttsp4_set_handshake_enable_exit:
	return retval;
}
#endif /* --CY_USE_TMA884 */

/*
 * change device mode - For example, change from
 * system information mode to operating mode
 */
static int _cyttsp4_set_device_mode(struct cyttsp4 *ts,
	u8 new_mode, u8 new_cur_mode, char *mode)
{
	u8 cmd = 0;
	int retval = 0;

	cmd = new_mode + CY_MODE_CHANGE;

	retval = _cyttsp4_put_cmd_wait(ts, CY_REG_BASE,
		sizeof(cmd), &cmd, CY_TEN_SEC_TMO_MS,
		_cyttsp4_chk_mode_change, &cmd,
		ts->platform_data->addr[CY_TCH_ADDR_OFS], true, CY_CMD_STATE);
	if (retval < 0) {
		dev_err(ts->dev,
			"%s: Fail Set mode command new_mode=%02X r=%d\n",
			__func__, new_mode, retval);
		goto _cyttsp4_set_device_mode_exit;
	}

	if (cmd != new_mode) {
		dev_err(ts->dev,
			"%s: failed to switch to %s mode\n", __func__, mode);
		retval = -EIO;
	} else {
		ts->current_mode = new_cur_mode;
		retval = _cyttsp4_handshake(ts, cmd);
		if (retval < 0) {
			dev_err(ts->dev,
			"%s: Fail handshake r=%d\n", __func__, retval);
			/* continue; rely on handshake tmo */
			retval = 0;
		}
	}

	dev_dbg(ts->dev,
		"%s: check op ready ret=%d host_mode=%02X\n",
		__func__, retval, cmd);

_cyttsp4_set_device_mode_exit:
	return retval;
}

static int _cyttsp4_set_mode(struct cyttsp4 *ts, u8 new_mode)
{
	enum cyttsp4_driver_state new_state = CY_TRANSFER_STATE;
	u8 new_cur_mode = CY_MODE_OPERATIONAL;
	char *mode = NULL;
	int retval = 0;

	switch (new_mode) {
	case CY_OPERATE_MODE:
		new_cur_mode = CY_MODE_OPERATIONAL;
		mode = "operational";
		INIT_COMPLETION(ts->ready_int_running);
		_cyttsp4_change_state(ts, CY_READY_STATE);
		new_state = CY_ACTIVE_STATE;
		break;
	case CY_SYSINFO_MODE:
		new_cur_mode = CY_MODE_SYSINFO;
		mode = "sysinfo";
		new_state = CY_SYSINFO_STATE;
		break;
	case CY_CONFIG_MODE:
		new_cur_mode = CY_MODE_OPERATIONAL;
		mode = "config";
		new_state = ts->driver_state;

		break;
	default:
		dev_err(ts->dev,
			"%s: invalid mode change request m=0x%02X\n",
			__func__, new_mode);
		retval = -EINVAL;
		goto _cyttsp_set_mode_exit;
	}

	retval = _cyttsp4_set_device_mode(ts,
		new_mode, new_cur_mode, mode);
	if (retval < 0) {
		dev_err(ts->dev,
			"%s: Fail switch to %s mode\n", __func__, mode);
		_cyttsp4_change_state(ts, CY_IDLE_STATE);
	} else {
		_cyttsp4_change_state(ts, new_state);
	}

_cyttsp_set_mode_exit:
	return retval;
}

#ifdef CY_USE_TMA884
static int _cyttsp4_write_config_block(struct cyttsp4 *ts, u8 blockid,
	const u8 *pdata, size_t ndata, u8 crc_h, u8 crc_l, const char *name)
{
	uint8_t *buf = NULL;
	size_t buf_size = 0;
	u8 status = 0;
	int retval = 0;
	bool lpe_local = ts->low_power_enable;

	ts->low_power_enable = false;

	/* pre-amble (10) + data (122) + crc (2) + key (8) */
	buf_size = sizeof(uint8_t) * 142;
	buf = kzalloc(buf_size, GFP_KERNEL);
	if (buf == NULL) {
		dev_err(ts->dev,
			"%s: Failed to allocate buffer for %s\n",
			__func__, name);
		retval = -ENOMEM;
		goto _cyttsp4_write_config_block_exit;
	}

	if (pdata == NULL) {
		dev_err(ts->dev,
			"%s: bad data pointer\n", __func__);
		retval = -ENXIO;
		goto _cyttsp4_write_config_block_exit;
	}

	if (ndata > 122) {
		dev_err(ts->dev,
			"%s: %s is too large n=%d size=%d\n",
			__func__, name, ndata, 122);
		retval = -EOVERFLOW;
		goto _cyttsp4_write_config_block_exit;
	}

	/* Set command bytes */
	buf[0] = 0x04; /* cmd */
	buf[1] = 0x00; /* row offset high */
	buf[2] = 0x00; /* row offset low */
	buf[3] = 0x00; /* write block length high */
	buf[4] = 0x80; /* write block length low */
	buf[5] = blockid; /* write block id */
	buf[6] = 0x00; /* num of config bytes + 4 high */
	buf[7] = 0x7E; /* num of config bytes + 4 low */
	buf[8] = 0x00; /* max block size w/o crc high */
	buf[9] = 0x7E; /* max block size w/o crc low */

	/* Copy platform data */
	memcpy(&(buf[10]), pdata, ndata);

	/* Copy block CRC */
	buf[132] = crc_h;
	buf[133] = crc_l;

	/* Set key bytes */
	buf[134] = 0x45;
	buf[135] = 0x63;
	buf[136] = 0x36;
	buf[137] = 0x6F;
	buf[138] = 0x34;
	buf[139] = 0x38;
	buf[140] = 0x73;
	buf[141] = 0x77;

	/* Write config block */
	_cyttsp4_pr_buf(ts, buf, buf_size, name);

	retval = _cyttsp4_write_block_data(ts, ts->si_ofs.cmd_ofs + 1,
		141, &(buf[1]),
		ts->platform_data->addr[CY_TCH_ADDR_OFS], true);
	if (retval < 0) {
		dev_err(ts->dev,
			"%s: Failed to write config %s r=%d\n",
			__func__, name, retval);
		goto _cyttsp4_write_config_block_exit;
	}

	retval = _cyttsp4_put_cmd_wait(ts, ts->si_ofs.cmd_ofs,
		1, &(buf[0]), CY_TEN_SEC_TMO_MS,
		_cyttsp4_chk_cmd_rdy, NULL,
		ts->platform_data->addr[CY_TCH_ADDR_OFS], true, CY_CMD_STATE);
	if (retval < 0) {
		dev_err(ts->dev,
			"%s: Fail write config command r=%d\n",
			__func__, retval);
		goto _cyttsp4_write_config_block_exit;
	}

	retval = _cyttsp4_read_block_data(ts, ts->si_ofs.cmd_ofs + 1,
		sizeof(status), &status,
		ts->platform_data->addr[CY_TCH_ADDR_OFS], true);
	if (retval < 0) {
		dev_err(ts->dev,
			"%s: Fail read status r=%d\n",
			__func__, retval);
		goto _cyttsp4_write_config_block_exit;
	}

	if (status != 0x00) {
		dev_err(ts->dev,
			"%s: Write config status=%d error\n",
			__func__, status);
		goto _cyttsp4_write_config_block_exit;
	}

_cyttsp4_write_config_block_exit:
	ts->low_power_enable = lpe_local;
	kfree(buf);
	return retval;
}
#endif /* --CY_USE_TMA884 */

#ifdef CONFIG_TOUCHSCREEN_DEBUG
#ifdef CY_USE_TMA884
static int _cyttsp4_read_config_block(struct cyttsp4 *ts, u8 blockid,
	u8 *pdata, size_t ndata, const char *name)
{
	int retval = 0;
	u8 cmd[CY_NUM_DAT+1];
	u8 status;

	/* Set command bytes */
	cmd[0] = 0x03; /* cmd */
	cmd[1] = 0x00; /* row offset high */
	cmd[2] = 0x00; /* row offset low */
	cmd[3] = ndata / 256; /* write block length high */
	cmd[4] = ndata % 256; /* write block length low */
	cmd[5] = blockid; /* read block id */
	cmd[6] = 0x00; /* blank fill */

	/* Write config block */
	_cyttsp4_pr_buf(ts, cmd, sizeof(cmd), name);

	retval = _cyttsp4_put_cmd_wait(ts, ts->si_ofs.cmd_ofs,
			sizeof(cmd), cmd, CY_TEN_SEC_TMO_MS,
			_cyttsp4_chk_cmd_rdy, NULL,
			ts->platform_data->addr[CY_TCH_ADDR_OFS], true, CY_CMD_STATE);
	if (retval < 0) {
		dev_err(ts->dev,
					"%s: Fail write config command r=%d\n",
					__func__, retval);
		goto _cyttsp4_read_config_block_exit;
	}

	if (pdata[1] != 0x00) {
		dev_err(ts->dev,
					"%s: Read config block command failed"
					" response=%02X %02X\n",
						__func__, pdata[0], pdata[1]);
		retval = -EIO;
	}
	retval = _cyttsp4_read_block_data(ts, ts->si_ofs.cmd_ofs + 1,
			sizeof(status), &status,
				ts->platform_data->addr[CY_TCH_ADDR_OFS], true);
	if (retval < 0) {
		dev_err(ts->dev,
					"%s: Fail read status r=%d\n",
						__func__, retval);
		goto _cyttsp4_read_config_block_exit;
	}

	if (status != 0x00) {
		dev_err(ts->dev,
					"%s: Write config status=%d error\n",
							__func__, status);
		goto _cyttsp4_read_config_block_exit;
	} else {
			memset(pdata, 0, ndata);
			retval = _cyttsp4_read_block_data(ts, ts->si_ofs.cmd_ofs,
						ndata, pdata,
							ts->platform_data->addr[CY_TCH_ADDR_OFS], true);
			if (retval < 0) {
					dev_err(ts->dev,
								"%s: fail read cmd status"
									" r=%d\n", __func__, retval);
			} else {
					/* write the returned raw read config block data */
					_cyttsp4_pr_buf(ts, pdata, ndata, name);
			}
	}
_cyttsp4_read_config_block_exit:
	return retval;
}
#endif /* --CY_USE_TMA884 */
#endif /* --CONFIG_TOUCHSCREEN_DEBUG */

#ifdef CY_USE_TMA884
#ifdef CY_AUTO_LOAD_TOUCH_PARAMS
static int _cyttsp4_set_op_params(struct cyttsp4 *ts, u8 crc_h, u8 crc_l)
{
	int retval = 0;

	if (ts->platform_data->sett[CY_IC_GRPNUM_TCH_PARM_VAL] == NULL) {
		dev_err(ts->dev,
			"%s: Missing Platform Touch Parameter"
			" values table\n", __func__);
		retval = -ENXIO;
		goto _cyttsp4_set_op_params_exit;
	}

	if ((ts->platform_data->sett
		[CY_IC_GRPNUM_TCH_PARM_VAL]->data == NULL) ||
		(ts->platform_data->sett
		[CY_IC_GRPNUM_TCH_PARM_VAL]->size == 0)) {
		dev_err(ts->dev,
			"%s: Missing Platform Touch Parameter"
			" values table data\n", __func__);
		retval = -ENXIO;
		goto _cyttsp4_set_op_params_exit;
	}

	/* Change to Config Mode */
	retval = _cyttsp4_set_mode(ts, CY_CONFIG_MODE);
	if (retval < 0) {
		dev_err(ts->dev,
			"%s: Failed to switch to config mode"
			" for touch params\n", __func__);
		goto _cyttsp4_set_op_params_exit;
	}
	retval = _cyttsp4_write_config_block(ts, CY_TCH_PARM_EBID,
		ts->platform_data->sett[CY_IC_GRPNUM_TCH_PARM_VAL]->data,
		ts->platform_data->sett[CY_IC_GRPNUM_TCH_PARM_VAL]->size,
		crc_h, crc_l, "platform_touch_param_data");

_cyttsp4_set_op_params_exit:
	return retval;
}
#endif /* --CY_AUTO_LOAD_TOUCH_PARAMS */

static int _cyttsp4_set_data_block(struct cyttsp4 *ts, u8 blkid, u8 *pdata,
	size_t ndata, const char *name, bool force, bool *data_updated)
{
	u8 data_crc[2];
	u8 ic_crc[2];
	int retval = 0;

	memset(data_crc, 0, sizeof(data_crc));
	memset(ic_crc, 0, sizeof(ic_crc));
	*data_updated = false;

	_cyttsp4_pr_buf(ts, pdata, ndata, name);

	dev_vdbg(ts->dev,
		"%s: calc %s crc\n", __func__, name);

	retval = _cyttsp4_calc_data_crc(ts, ndata, pdata,
		&data_crc[0], &data_crc[1],
		name);
	if (retval < 0) {
		dev_err(ts->dev,
			"%s: fail calc crc for %s (0x%02X%02X) r=%d\n",
			__func__, name,
			data_crc[0], data_crc[1],
			retval);
		goto _cyttsp_set_data_block_exit;
	}

	dev_vdbg(ts->dev,
		"%s: get ic %s crc\n", __func__, name);
	retval = _cyttsp4_set_mode(ts, CY_OPERATE_MODE);
	if (retval < 0) {
		dev_err(ts->dev,
			"%s: Failed to switch to operational mode\n", __func__);
		goto _cyttsp_set_data_block_exit;
	}

	retval = _cyttsp4_get_ic_crc(ts, blkid,
		&ic_crc[0], &ic_crc[1]);
	if (retval < 0) {
		dev_err(ts->dev,
			"%s: fail get ic crc for %s (0x%02X%02X) r=%d\n",
			__func__, name,
			ic_crc[0], ic_crc[1],
			retval);
		goto _cyttsp_set_data_block_exit;
	}

	dev_vdbg(ts->dev,
		"%s: %s calc_crc=0x%02X%02X ic_crc=0x%02X%02X\n",
		__func__, name,
		data_crc[0], data_crc[1],
		ic_crc[0], ic_crc[1]);
	if ((data_crc[0] != ic_crc[0]) || (data_crc[1] != ic_crc[1]) || force) {
		/* Change to Config Mode */
		retval = _cyttsp4_set_mode(ts, CY_CONFIG_MODE);
		if (retval < 0) {
			dev_err(ts->dev,
			"%s: Failed to switch to config mode"
				" for sysinfo regs\n", __func__);
			goto _cyttsp_set_data_block_exit;
		}

		retval = _cyttsp4_write_config_block(ts, blkid, pdata,
			ndata, data_crc[0], data_crc[1], name);
		if (retval < 0) {
			dev_err(ts->dev,
			"%s: fail write %s config block r=%d\n",
				__func__, name, retval);
			goto _cyttsp_set_data_block_exit;
		}

		dev_vdbg(ts->dev,
			"%s: write %s config block ok\n", __func__, name);
		*data_updated = true;
	}

_cyttsp_set_data_block_exit:
	return retval;
}

static int _cyttsp4_set_sysinfo_regs(struct cyttsp4 *ts, bool *updated)
{
	bool ddata_updated = false;
	bool mdata_updated = false;
#if defined(CY_AUTO_LOAD_DDATA) || defined(CY_AUTO_LOAD_MDATA)
	size_t num_data = 0;
#endif /* --CY_AUTO_LOAD_DDATA || --CY_AUTO_LOAD_DDATA */
	u8 *pdata = NULL;
	int retval = 0;

	pdata = kzalloc(CY_NUM_MDATA, GFP_KERNEL);
	if (pdata == NULL) {
		dev_err(ts->dev,
			"%s: fail allocate set sysinfo regs buffer\n",
			__func__);
		retval = -ENOMEM;
		goto _cyttsp4_set_sysinfo_regs_err;
	}

#ifdef CY_AUTO_LOAD_DDATA
	/* check for missing DDATA */
	if (ts->platform_data->sett[CY_IC_GRPNUM_DDATA_REC] == NULL) {
		dev_vdbg(ts->dev,
			"%s: No platform_ddata table\n", __func__);
		dev_vdbg(ts->dev,
			"%s: Use a zero filled array to compare with device\n",
			__func__);
		goto _cyttsp4_set_sysinfo_regs_set_ddata_block;
	}
	if ((ts->platform_data->sett[CY_IC_GRPNUM_DDATA_REC]->data == NULL) ||
		(ts->platform_data->sett[CY_IC_GRPNUM_DDATA_REC]->size == 0)) {
		dev_vdbg(ts->dev,
			"%s: No platform_ddata table data\n", __func__);
		dev_vdbg(ts->dev,
			"%s: Use a zero filled array to compare with device\n",
			__func__);
		goto _cyttsp4_set_sysinfo_regs_set_ddata_block;
	}

	/* copy platform data design data to the device eeprom */
	num_data = ts->platform_data->sett
		[CY_IC_GRPNUM_DDATA_REC]->size < CY_NUM_DDATA ?
		ts->platform_data->sett
		[CY_IC_GRPNUM_DDATA_REC]->size : CY_NUM_DDATA;
	dev_vdbg(ts->dev,
		"%s: copy %d bytes from platform data to ddata array\n",
		__func__, num_data);
	memcpy(pdata, ts->platform_data->sett[CY_IC_GRPNUM_DDATA_REC]->data,
		num_data);

_cyttsp4_set_sysinfo_regs_set_ddata_block:
	/* set data block will check CRC match/nomatch */
	retval = _cyttsp4_set_data_block(ts, CY_DDATA_EBID, pdata,
		CY_NUM_DDATA, "platform_ddata", false, &ddata_updated);
	if (retval < 0) {
		dev_err(ts->dev,
			"%s: Fail while writing platform_ddata"
			" block to ic r=%d\n", __func__, retval);
	}
#else
	ddata_updated = false;
#endif /* --CY_AUTO_LOAD_DDATA */

#ifdef CY_AUTO_LOAD_MDATA
	/* check for missing MDATA */
	if (ts->platform_data->sett[CY_IC_GRPNUM_MDATA_REC] == NULL) {
		dev_vdbg(ts->dev,
			"%s: No platform_mdata table\n", __func__);
		dev_vdbg(ts->dev,
			"%s: Use a zero filled array to compare with device\n",
			__func__);
		goto _cyttsp4_set_sysinfo_regs_set_mdata_block;
	}
	if ((ts->platform_data->sett[CY_IC_GRPNUM_MDATA_REC]->data == NULL) ||
		(ts->platform_data->sett[CY_IC_GRPNUM_MDATA_REC]->size == 0)) {
		dev_vdbg(ts->dev,
			"%s: No platform_mdata table data\n", __func__);
		dev_vdbg(ts->dev,
			"%s: Use a zero filled array to compare with device\n",
			__func__);
		goto _cyttsp4_set_sysinfo_regs_set_mdata_block;
	}

	/* copy platform manufacturing data to the device eeprom */
	num_data = ts->platform_data->sett
		[CY_IC_GRPNUM_MDATA_REC]->size < CY_NUM_MDATA ?
		ts->platform_data->sett
		[CY_IC_GRPNUM_MDATA_REC]->size : CY_NUM_MDATA;
	dev_vdbg(ts->dev,
		"%s: copy %d bytes from platform data to mdata array\n",
		__func__, num_data);
	memcpy(pdata, ts->platform_data->sett[CY_IC_GRPNUM_MDATA_REC]->data,
		num_data);

_cyttsp4_set_sysinfo_regs_set_mdata_block:
	/* set data block will check CRC match/nomatch */
	retval = _cyttsp4_set_data_block(ts, CY_MDATA_EBID, pdata,
		CY_NUM_MDATA, "platform_mdata", false, &mdata_updated);
	if (retval < 0) {
		dev_err(ts->dev,
			"%s: Fail while writing platform_mdata"
			" block to ic r=%d\n", __func__, retval);
	}
#else
	mdata_updated = false;
#endif /* --CY_AUTO_LOAD_MDATA */

	kfree(pdata);
_cyttsp4_set_sysinfo_regs_err:
	*updated = ddata_updated || mdata_updated;
	return retval;
}
#endif /* --CY_USE_TMA884 */

static int _cyttsp4_bits_2_bytes(struct cyttsp4 *ts, int nbits, int *max)
{
	int nbytes;

	*max = 1 << nbits;

	for (nbytes = 0; nbits > 0;) {
		dev_vdbg(ts->dev,
			"%s: nbytes=%d nbits=%d\n", __func__, nbytes, nbits);
		nbytes++;
		if (nbits > 8)
			nbits -= 8;
		else
			nbits = 0;
		dev_vdbg(ts->dev,
			"%s: nbytes=%d nbits=%d\n", __func__, nbytes, nbits);
	}

	return nbytes;
}

static int _cyttsp4_get_sysinfo_regs(struct cyttsp4 *ts)
{
	int btn = 0;
	int num_defined_keys = 0;
	u16 *key_table = NULL;
	enum cyttsp4_tch_abs abs = 0;
	int retval = 0;

	/* pre-clear si_ofs structure */
	memset(&ts->si_ofs, 0, sizeof(struct cyttsp4_sysinfo_ofs));

	/* get the sysinfo data offsets */
	retval = _cyttsp4_read_block_data(ts, CY_REG_BASE,
		sizeof(ts->sysinfo_data), &(ts->sysinfo_data),
		ts->platform_data->addr[CY_TCH_ADDR_OFS], true);
	if (retval < 0) {
		dev_err(ts->dev,
			"%s: fail read sysinfo data offsets r=%d\n",
			__func__, retval);
		goto _cyttsp4_get_sysinfo_regs_exit_no_handshake;
	} else {
		/* Print sysinfo data offsets */
		_cyttsp4_pr_buf(ts, (u8 *)&ts->sysinfo_data,
			sizeof(ts->sysinfo_data), "sysinfo_data_offsets");

		/* convert sysinfo data offset bytes into integers */
		ts->si_ofs.map_sz = (ts->sysinfo_data.map_szh * 256) +
			ts->sysinfo_data.map_szl;
		ts->si_ofs.cydata_ofs = (ts->sysinfo_data.cydata_ofsh * 256) +
			ts->sysinfo_data.cydata_ofsl;
		ts->si_ofs.test_ofs = (ts->sysinfo_data.test_ofsh * 256) +
			ts->sysinfo_data.test_ofsl;
		ts->si_ofs.pcfg_ofs = (ts->sysinfo_data.pcfg_ofsh * 256) +
			ts->sysinfo_data.pcfg_ofsl;
		ts->si_ofs.opcfg_ofs = (ts->sysinfo_data.opcfg_ofsh * 256) +
			ts->sysinfo_data.opcfg_ofsl;
		ts->si_ofs.ddata_ofs = (ts->sysinfo_data.ddata_ofsh * 256) +
			ts->sysinfo_data.ddata_ofsl;
		ts->si_ofs.mdata_ofs = (ts->sysinfo_data.mdata_ofsh * 256) +
			ts->sysinfo_data.mdata_ofsl;
	}

	/* get the sysinfo cydata */
	ts->si_ofs.cydata_size = ts->si_ofs.test_ofs - ts->si_ofs.cydata_ofs;
	ts->sysinfo_ptr.cydata = kzalloc(ts->si_ofs.cydata_size, GFP_KERNEL);
	if (ts->sysinfo_ptr.cydata == NULL) {
		retval = -ENOMEM;
		dev_err(ts->dev,
			"%s: fail alloc cydata memory r=%d\n",
			__func__, retval);
		goto _cyttsp4_get_sysinfo_regs_exit;
	} else {
		memset(ts->sysinfo_ptr.cydata, 0, ts->si_ofs.cydata_size);
		retval = _cyttsp4_read_block_data(ts, ts->si_ofs.cydata_ofs,
			ts->si_ofs.cydata_size, ts->sysinfo_ptr.cydata,
			ts->platform_data->addr[CY_TCH_ADDR_OFS], true);
		if (retval < 0) {
			dev_err(ts->dev,
			"%s: fail read cydata r=%d\n",
				__func__, retval);
			goto _cyttsp4_get_sysinfo_regs_exit_no_handshake;
		}
		/* Print sysinfo cydata */
		_cyttsp4_pr_buf(ts, (u8 *)ts->sysinfo_ptr.cydata,
			ts->si_ofs.cydata_size, "sysinfo_cydata");
	}
	/* get the sysinfo test data */
	ts->si_ofs.test_size = ts->si_ofs.pcfg_ofs - ts->si_ofs.test_ofs;
	ts->sysinfo_ptr.test = kzalloc(ts->si_ofs.test_size, GFP_KERNEL);
	if (ts->sysinfo_ptr.test == NULL) {
		retval = -ENOMEM;
		dev_err(ts->dev,
			"%s: fail alloc test memory r=%d\n",
			__func__, retval);
		goto _cyttsp4_get_sysinfo_regs_exit;
	} else {
		memset(ts->sysinfo_ptr.test, 0, ts->si_ofs.test_size);
		retval = _cyttsp4_read_block_data(ts, ts->si_ofs.test_ofs,
			ts->si_ofs.test_size, ts->sysinfo_ptr.test,
			ts->platform_data->addr[CY_TCH_ADDR_OFS], true);
		if (retval < 0) {
			dev_err(ts->dev,
			"%s: fail read test data r=%d\n",
				__func__, retval);
			goto _cyttsp4_get_sysinfo_regs_exit;
		}
		/* Print sysinfo test data */
		_cyttsp4_pr_buf(ts, (u8 *)ts->sysinfo_ptr.test,
			ts->si_ofs.test_size, "sysinfo_test_data");
	}
	/* get the sysinfo pcfg data */
	ts->si_ofs.pcfg_size = ts->si_ofs.opcfg_ofs - ts->si_ofs.pcfg_ofs;
	ts->sysinfo_ptr.pcfg = kzalloc(ts->si_ofs.pcfg_size, GFP_KERNEL);
	if (ts->sysinfo_ptr.pcfg == NULL) {
		retval = -ENOMEM;
		dev_err(ts->dev,
			"%s: fail alloc pcfg memory r=%d\n",
			__func__, retval);
		goto _cyttsp4_get_sysinfo_regs_exit;
	} else {
		memset(ts->sysinfo_ptr.pcfg, 0, ts->si_ofs.pcfg_size);
		retval = _cyttsp4_read_block_data(ts, ts->si_ofs.pcfg_ofs,
			ts->si_ofs.pcfg_size, ts->sysinfo_ptr.pcfg,
			ts->platform_data->addr[CY_TCH_ADDR_OFS], true);
		if (retval < 0) {
			dev_err(ts->dev,
			"%s: fail read pcfg data r=%d\n",
				__func__, retval);
			goto _cyttsp4_get_sysinfo_regs_exit;
		}
		/* Print sysinfo pcfg data */
		_cyttsp4_pr_buf(ts, (u8 *)ts->sysinfo_ptr.pcfg,
			ts->si_ofs.pcfg_size, "sysinfo_pcfg_data");
	}
	/* get the sysinfo opcfg data */
	ts->si_ofs.opcfg_size = ts->si_ofs.ddata_ofs - ts->si_ofs.opcfg_ofs;
	ts->sysinfo_ptr.opcfg = kzalloc(ts->si_ofs.opcfg_size, GFP_KERNEL);
	if (ts->sysinfo_ptr.opcfg == NULL) {
		retval = -ENOMEM;
		dev_err(ts->dev,
			"%s: fail alloc opcfg memory r=%d\n",
			__func__, retval);
		goto _cyttsp4_get_sysinfo_regs_exit;
	} else {
		memset(ts->sysinfo_ptr.opcfg, 0, ts->si_ofs.opcfg_size);
		retval = _cyttsp4_read_block_data(ts, ts->si_ofs.opcfg_ofs,
			ts->si_ofs.opcfg_size, ts->sysinfo_ptr.opcfg,
			ts->platform_data->addr[CY_TCH_ADDR_OFS], true);
		if (retval < 0) {
			dev_err(ts->dev,
			"%s: fail read opcfg data r=%d\n",
				__func__, retval);
			goto _cyttsp4_get_sysinfo_regs_exit;
		}
		ts->si_ofs.cmd_ofs = ts->sysinfo_ptr.opcfg->cmd_ofs;
		ts->si_ofs.rep_ofs = ts->sysinfo_ptr.opcfg->rep_ofs;
		ts->si_ofs.rep_sz = (ts->sysinfo_ptr.opcfg->rep_szh * 256) +
			ts->sysinfo_ptr.opcfg->rep_szl;
		ts->si_ofs.num_btns = ts->sysinfo_ptr.opcfg->num_btns;
		if (ts->si_ofs.num_btns == 0)
			ts->si_ofs.num_btn_regs = 0;
		else {
			ts->si_ofs.num_btn_regs = ts->si_ofs.num_btns /
				CY_NUM_BTN_PER_REG;
			if (ts->si_ofs.num_btns % CY_NUM_BTN_PER_REG)
				ts->si_ofs.num_btn_regs++;
		}
		ts->si_ofs.tt_stat_ofs = ts->sysinfo_ptr.opcfg->tt_stat_ofs;
		ts->si_ofs.obj_cfg0 = ts->sysinfo_ptr.opcfg->obj_cfg0;
		ts->si_ofs.max_tchs = ts->sysinfo_ptr.opcfg->max_tchs &
				CY_BYTE_OFS_MASK;
		ts->si_ofs.tch_rec_siz = ts->sysinfo_ptr.opcfg->tch_rec_siz &
				CY_BYTE_OFS_MASK;

		/* Get the old touch fields */
		for (abs = CY_TCH_X; abs < CY_NUM_OLD_TCH_FIELDS; abs++) {
			ts->si_ofs.tch_abs[abs].ofs =

			ts->sysinfo_ptr.opcfg->tch_rec_old[abs].loc &
				CY_BYTE_OFS_MASK;

			ts->si_ofs.tch_abs[abs].size =
				_cyttsp4_bits_2_bytes(ts,

			ts->sysinfo_ptr.opcfg->tch_rec_old[abs].size,
				&ts->si_ofs.tch_abs[abs].max);

			ts->si_ofs.tch_abs[abs].bofs =
				(ts->sysinfo_ptr.opcfg->tch_rec_old[abs].loc &
				CY_BOFS_MASK) >> CY_BOFS_SHIFT;

			dev_vdbg(ts->dev,
				"%s: tch_rec_%s\n", __func__,
				cyttsp4_tch_abs_string[abs]);
			dev_vdbg(ts->dev,
				"%s:     ofs =%2d\n", __func__,
				ts->si_ofs.tch_abs[abs].ofs);
			dev_vdbg(ts->dev,
				"%s:     siz =%2d\n", __func__,
				ts->si_ofs.tch_abs[abs].size);
			dev_vdbg(ts->dev,
				"%s:     max =%2d\n", __func__,
				ts->si_ofs.tch_abs[abs].max);
			dev_vdbg(ts->dev,
				"%s:     bofs=%2d\n", __func__,
				ts->si_ofs.tch_abs[abs].bofs);
		}

		ts->si_ofs.btn_rec_siz = ts->sysinfo_ptr.opcfg->btn_rec_siz;
		ts->si_ofs.btn_diff_ofs = ts->sysinfo_ptr.opcfg->btn_diff_ofs;
		ts->si_ofs.btn_diff_siz = ts->sysinfo_ptr.opcfg->btn_diff_siz;
		ts->si_ofs.mode_size = ts->si_ofs.tt_stat_ofs + 1;
		ts->si_ofs.data_size = ts->si_ofs.max_tchs *
			ts->sysinfo_ptr.opcfg->tch_rec_siz;
		if (ts->si_ofs.num_btns)
			ts->si_ofs.mode_size += ts->si_ofs.num_btn_regs;

		/* Print sysinfo opcfg data */
		_cyttsp4_pr_buf(ts, (u8 *)ts->sysinfo_ptr.opcfg,
			ts->si_ofs.opcfg_size, "sysinfo_opcfg_data");
	}

	/* get the sysinfo ddata data */
	ts->si_ofs.ddata_size = ts->si_ofs.mdata_ofs - ts->si_ofs.ddata_ofs;
	ts->sysinfo_ptr.ddata = kzalloc(ts->si_ofs.ddata_size, GFP_KERNEL);
	if (ts->sysinfo_ptr.ddata == NULL) {
		dev_err(ts->dev,
			"%s: fail alloc ddata memory r=%d\n",
			__func__, retval);
		/* continue */
	} else {
		memset(ts->sysinfo_ptr.ddata, 0, ts->si_ofs.ddata_size);
		retval = _cyttsp4_read_block_data(ts, ts->si_ofs.ddata_ofs,
			ts->si_ofs.ddata_size, ts->sysinfo_ptr.ddata,
			ts->platform_data->addr[CY_TCH_ADDR_OFS], true);
		if (retval < 0) {
			dev_err(ts->dev,
			"%s: fail read ddata data r=%d\n",
				__func__, retval);
			goto _cyttsp4_get_sysinfo_regs_exit;
		}
		/* Print sysinfo ddata */
		_cyttsp4_pr_buf(ts, (u8 *)ts->sysinfo_ptr.ddata,
			ts->si_ofs.ddata_size, "sysinfo_ddata");
	}
	/* get the sysinfo mdata data */
	ts->si_ofs.mdata_size = ts->si_ofs.map_sz - ts->si_ofs.mdata_ofs;
	ts->sysinfo_ptr.mdata = kzalloc(ts->si_ofs.mdata_size, GFP_KERNEL);
	if (ts->sysinfo_ptr.mdata == NULL) {
		dev_err(ts->dev,
			"%s: fail alloc mdata memory r=%d\n",
			__func__, retval);
		/* continue */
	} else {
		memset(ts->sysinfo_ptr.mdata, 0, ts->si_ofs.mdata_size);
		retval = _cyttsp4_read_block_data(ts, ts->si_ofs.mdata_ofs,
			ts->si_ofs.mdata_size, ts->sysinfo_ptr.mdata,
			ts->platform_data->addr[CY_TCH_ADDR_OFS], true);
		if (retval < 0) {
			dev_err(ts->dev,
			"%s: fail read mdata data r=%d\n",
				__func__, retval);
			goto _cyttsp4_get_sysinfo_regs_exit;
		}
		/* Print sysinfo mdata */
		_cyttsp4_pr_buf(ts, (u8 *)ts->sysinfo_ptr.mdata,
			ts->si_ofs.mdata_size, "sysinfo_mdata");
	}

	if (ts->si_ofs.num_btns) {
		ts->si_ofs.btn_keys_size = ts->si_ofs.num_btns *
			sizeof(struct cyttsp4_btn);
		ts->btn = kzalloc(ts->si_ofs.btn_keys_size, GFP_KERNEL);
		if (ts->btn == NULL) {
			dev_err(ts->dev,
			"%s: fail alloc btn_keys memory r=%d\n",
				__func__, retval);
		} else {
			if (ts->platform_data->sett
				[CY_IC_GRPNUM_BTN_KEYS] == NULL)
				num_defined_keys = 0;
			else if (ts->platform_data->sett
				[CY_IC_GRPNUM_BTN_KEYS]->data == NULL)
				num_defined_keys = 0;
			else
				num_defined_keys = ts->platform_data->sett
					[CY_IC_GRPNUM_BTN_KEYS]->size;
			for (btn = 0; btn < ts->si_ofs.num_btns &&
				btn < num_defined_keys; btn++) {
				key_table = (u16 *)ts->platform_data->sett
					[CY_IC_GRPNUM_BTN_KEYS]->data;
				ts->btn[btn].key_code = key_table[btn];
				ts->btn[btn].enabled = true;
			}
			for (; btn < ts->si_ofs.num_btns; btn++) {
				ts->btn[btn].key_code = KEY_RESERVED;
				ts->btn[btn].enabled = true;
			}
		}
	} else {
		ts->si_ofs.btn_keys_size = 0;
		ts->btn = NULL;
	}

	dev_vdbg(ts->dev,
		"%s: cydata_ofs =%4d siz=%4d\n", __func__,
		ts->si_ofs.cydata_ofs, ts->si_ofs.cydata_size);
	dev_vdbg(ts->dev,
		"%s: test_ofs   =%4d siz=%4d\n", __func__,
		ts->si_ofs.test_ofs, ts->si_ofs.test_size);
	dev_vdbg(ts->dev,
		"%s: pcfg_ofs   =%4d siz=%4d\n", __func__,
		ts->si_ofs.pcfg_ofs, ts->si_ofs.pcfg_size);
	dev_vdbg(ts->dev,
		"%s: opcfg_ofs  =%4d siz=%4d\n", __func__,
		ts->si_ofs.opcfg_ofs, ts->si_ofs.opcfg_size);
	dev_vdbg(ts->dev,
		"%s: ddata_ofs  =%4d siz=%4d\n", __func__,
		ts->si_ofs.ddata_ofs, ts->si_ofs.ddata_size);
	dev_vdbg(ts->dev,
		"%s: mdata_ofs  =%4d siz=%4d\n", __func__,
		ts->si_ofs.mdata_ofs, ts->si_ofs.mdata_size);

	dev_vdbg(ts->dev,
		"%s: cmd_ofs       =%4d\n", __func__, ts->si_ofs.cmd_ofs);
	dev_vdbg(ts->dev,
		"%s: rep_ofs       =%4d\n", __func__, ts->si_ofs.rep_ofs);
	dev_vdbg(ts->dev,
		"%s: rep_sz        =%4d\n", __func__, ts->si_ofs.rep_sz);
	dev_vdbg(ts->dev,
		"%s: num_btns      =%4d\n", __func__, ts->si_ofs.num_btns);
	dev_vdbg(ts->dev,
		"%s: num_btn_regs  =%4d\n", __func__, ts->si_ofs.num_btn_regs);
	dev_vdbg(ts->dev,
		"%s: tt_stat_ofs   =%4d\n", __func__, ts->si_ofs.tt_stat_ofs);
	dev_vdbg(ts->dev,
		"%s: tch_rec_siz   =%4d\n", __func__, ts->si_ofs.tch_rec_siz);
	dev_vdbg(ts->dev,
		"%s: max_tchs      =%4d\n", __func__, ts->si_ofs.max_tchs);
	dev_vdbg(ts->dev,
		"%s: mode_siz      =%4d\n", __func__, ts->si_ofs.mode_size);
	dev_vdbg(ts->dev,
		"%s: data_siz      =%4d\n", __func__, ts->si_ofs.data_size);
	dev_vdbg(ts->dev,
		"%s: map_sz        =%4d\n", __func__, ts->si_ofs.map_sz);

	dev_vdbg(ts->dev,
		"%s: btn_rec_siz   =%2d\n", __func__, ts->si_ofs.btn_rec_siz);
	dev_vdbg(ts->dev,
		"%s: btn_diff_ofs  =%2d\n", __func__, ts->si_ofs.btn_diff_ofs);
	dev_vdbg(ts->dev,
		"%s: btn_diff_siz  =%2d\n", __func__, ts->si_ofs.btn_diff_siz);

	dev_vdbg(ts->dev,
		"%s: mode_size     =%2d\n", __func__, ts->si_ofs.mode_size);
	dev_vdbg(ts->dev,
		"%s: data_size     =%2d\n", __func__, ts->si_ofs.data_size);

	if (ts->xy_mode == NULL)
		ts->xy_mode = kzalloc(ts->si_ofs.mode_size, GFP_KERNEL);
	if (ts->xy_data == NULL)
		ts->xy_data = kzalloc(ts->si_ofs.data_size, GFP_KERNEL);
	if (ts->xy_data_touch1 == NULL) {
		ts->xy_data_touch1 = kzalloc(ts->si_ofs.tch_rec_siz + 1,
			GFP_KERNEL);
	}
	if (ts->btn_rec_data == NULL) {
		ts->btn_rec_data = kzalloc(ts->si_ofs.btn_rec_siz *
			ts->si_ofs.num_btns, GFP_KERNEL);
	}
	if ((ts->xy_mode == NULL) || (ts->xy_data == NULL) ||
		(ts->xy_data_touch1 == NULL) || (ts->btn_rec_data == NULL)) {
		dev_err(ts->dev,
			"%s: fail memory alloc xy_mode=%p xy_data=%p"
			"xy_data_touch1=%p btn_rec_data=%p\n", __func__,
			ts->xy_mode, ts->xy_data,
			ts->xy_data_touch1, ts->btn_rec_data);
		/* continue */
	}

	dev_vdbg(ts->dev,
		"%s: xy_mode=%p xy_data=%p xy_data_touch1=%p\n",
		__func__, ts->xy_mode, ts->xy_data, ts->xy_data_touch1);

_cyttsp4_get_sysinfo_regs_exit:
	/* provide flow control handshake */
	retval = _cyttsp4_handshake(ts, ts->sysinfo_data.hst_mode);
	if (retval < 0) {
		dev_err(ts->dev,
			"%s: handshake fail on sysinfo reg\n",
			__func__);
		/* continue; rely on handshake tmo */
	}

_cyttsp4_get_sysinfo_regs_exit_no_handshake:
	return retval;
}

static int _cyttsp4_load_status_regs(struct cyttsp4 *ts)
{
	int rep_stat_ofs = 0;
	int retval = 0;

	rep_stat_ofs = ts->si_ofs.rep_ofs + 1;
	if (ts->xy_mode == NULL) {
		dev_err(ts->dev,
			"%s: mode ptr not yet initialized xy_mode=%p\n",
			__func__, ts->xy_mode);
		/* continue */
	} else {
		retval = _cyttsp4_read_block_data(ts, CY_REG_BASE,
			ts->si_ofs.mode_size, ts->xy_mode,
			ts->platform_data->addr[CY_TCH_ADDR_OFS], true);
		if (retval < 0) {
			dev_err(ts->dev,
			"%s: fail read mode regs r=%d\n",
				__func__, retval);
			retval = -EIO;
		}
		_cyttsp4_pr_buf(ts, ts->xy_mode, ts->si_ofs.mode_size,
			"xy_mode");
	}
	return retval;
}

static void _cyttsp4_btn_key_release(struct cyttsp4 *ts,
	int cur_btn, u8 cur_btn_mask, int num_btns)
{
	int btn = 0;

	/* Check for button releases */
	for (btn = 0; btn < num_btns; btn++) {
		if (ts->btn[cur_btn + btn].enabled) {
			switch ((cur_btn_mask >> (btn * CY_BITS_PER_BTN)) &
				(CY_NUM_BTN_EVENT_ID - 1)) {
			case (CY_BTN_RELEASED):
				if (ts->btn[cur_btn + btn].state ==
					CY_BTN_PRESSED) {
					input_report_key(ts->input,
						ts->btn[cur_btn + btn].key_code,
						CY_BTN_RELEASED);
					ts->btn[cur_btn + btn].state =
						CY_BTN_RELEASED;
					input_sync(ts->input);
					dev_dbg(ts->dev,
						"%s: btn=%d key_code=%d"
						" RELEASED\n", __func__,
						cur_btn + btn, ts->btn
						[cur_btn + btn].key_code);
				}
				break;
			case (CY_BTN_PRESSED):
				break;
			default:
				break;
			}
		}
	}
	return;
}

static void _cyttsp4_btn_key_press(struct cyttsp4 *ts,
	int cur_btn, u8 cur_btn_mask, int num_btns)
{
	int btn = 0;

	/* Check for button presses */
	for (btn = 0; btn < num_btns; btn++) {
		if (ts->btn[cur_btn + btn].enabled) {
			switch ((cur_btn_mask >> (btn * CY_BITS_PER_BTN)) &
				(CY_NUM_BTN_EVENT_ID - 1)) {
			case (CY_BTN_RELEASED):
				break;
			case (CY_BTN_PRESSED):
				if (ts->btn[cur_btn + btn].state ==
					CY_BTN_RELEASED) {
					input_report_key(ts->input,
						ts->btn[cur_btn + btn].key_code,
						CY_BTN_PRESSED);
					ts->btn[cur_btn + btn].state =
						CY_BTN_PRESSED;
					input_sync(ts->input);
					dev_dbg(ts->dev,
						"%s: btn=%d key_code=%d"
						" PRESSED\n", __func__,
						cur_btn + btn, ts->btn
						[cur_btn + btn].key_code);
				}
				break;
			default:
				break;
			}
		}
	}
	return;
}

static void _cyttsp4_get_touch_axis(struct cyttsp4 *ts,
	enum cyttsp4_tch_abs abs, int *axis, int size,
	int max, u8 *xy_data, int bofs)
{
	int nbyte = 0;
	int next = 0;

	for (nbyte = 0, *axis = 0, next = 0; nbyte < size; nbyte++) {
		dev_vdbg(ts->dev,
			"%s: *axis=%02X(%d) size=%d max=%08X xy_data=%p"
			" xy_data[%d]=%02X(%d)\n",
			__func__, *axis, *axis, size, max, xy_data, next,
			xy_data[next], xy_data[next]);
		*axis = (*axis * 256) + (xy_data[next] >> bofs);
		next++;
	}

	*axis &= max - 1;

	dev_vdbg(ts->dev,
		"%s: *axis=%02X(%d) size=%d max=%08X xy_data=%p"
		" xy_data[%d]=%02X(%d)\n",
		__func__, *axis, *axis, size, max, xy_data, next,
		xy_data[next], xy_data[next]);
}

static void _cyttsp4_get_touch(struct cyttsp4 *ts,
	struct cyttsp4_touch *touch, u8 *xy_data)
{
	enum cyttsp4_tch_abs abs = 0;
#ifdef CY_USE_DEBUG_TOOLS
	int tmp = 0;
	bool flipped = false;
#endif /* --CY_USE_DEBUG_TOOLS */

	for (abs = CY_TCH_X; abs < CY_TCH_NUM_ABS; abs++) {
		_cyttsp4_get_touch_axis(ts, abs, &touch->abs[abs],
			ts->si_ofs.tch_abs[abs].size,
			ts->si_ofs.tch_abs[abs].max,
			xy_data + ts->si_ofs.tch_abs[abs].ofs,
			ts->si_ofs.tch_abs[abs].bofs);
		dev_vdbg(ts->dev,
			"%s: get %s=%08X(%d) size=%d"
			" ofs=%d max=%d xy_data+ofs=%p bofs=%d\n",
			__func__, cyttsp4_tch_abs_string[abs],
			touch->abs[abs], touch->abs[abs],
			ts->si_ofs.tch_abs[abs].size,
			ts->si_ofs.tch_abs[abs].ofs,
			ts->si_ofs.tch_abs[abs].max,
			xy_data + ts->si_ofs.tch_abs[abs].ofs,
			ts->si_ofs.tch_abs[abs].bofs);
	}

#ifdef CY_USE_DEBUG_TOOLS
	if (ts->flags & CY_FLAG_FLIP) {
		tmp = touch->abs[CY_TCH_X];
		touch->abs[CY_TCH_X] =
			touch->abs[CY_TCH_Y];
		touch->abs[CY_TCH_Y] = tmp;
		flipped = true;
	}
	if (ts->flags & CY_FLAG_INV_X) {
		if (!flipped) {
			touch->abs[CY_TCH_X] =
				ts->platform_data->frmwrk->abs
				[(CY_ABS_X_OST * CY_NUM_ABS_SET) + CY_MAX_OST] -
				touch->abs[CY_TCH_X];
		} else {
			touch->abs[CY_TCH_X] =
				ts->platform_data->frmwrk->abs
				[(CY_ABS_Y_OST * CY_NUM_ABS_SET) + CY_MAX_OST] -
				touch->abs[CY_TCH_X];
		}
	}
	if (ts->flags & CY_FLAG_INV_Y) {
		if (!flipped) {
			touch->abs[CY_TCH_Y] =
				ts->platform_data->frmwrk->abs
				[(CY_ABS_Y_OST * CY_NUM_ABS_SET) + CY_MAX_OST] -
				touch->abs[CY_TCH_Y];
		} else {
			touch->abs[CY_TCH_Y] =
				ts->platform_data->frmwrk->abs
				[(CY_ABS_X_OST * CY_NUM_ABS_SET) + CY_MAX_OST] -
				touch->abs[CY_TCH_Y];
		}
	}
#endif /* --CY_USE_DEBUG_TOOLS */
}

static void _cyttsp4_get_mt_touches(struct cyttsp4 *ts, int num_cur_tch)
{
	struct cyttsp4_touch touch;
	int signal = CY_IGNORE_VALUE;
	int i = 0;
	int j = 0;
	int t = 0;
	int mt_sync_count = 0;

	memset(&touch, 0, sizeof(struct cyttsp4_touch));
	for (i = 0; i < num_cur_tch; i++) {
		_cyttsp4_get_touch(ts, &touch,
			ts->xy_data + (i * ts->si_ofs.tch_rec_siz));
		if ((touch.abs[CY_TCH_T] < ts->platform_data->frmwrk->abs
			[(CY_ABS_ID_OST * CY_NUM_ABS_SET) + CY_MIN_OST]) ||
			(touch.abs[CY_TCH_T] > ts->platform_data->frmwrk->abs
			[(CY_ABS_ID_OST * CY_NUM_ABS_SET) + CY_MAX_OST])) {
			dev_err(ts->dev,
				"%s: touch=%d has bad track_id=%d max_id=%d\n",
				__func__, i, touch.abs[CY_TCH_T],
				ts->platform_data->frmwrk->abs
				[(CY_ABS_ID_OST * CY_NUM_ABS_SET) +
				CY_MAX_OST]);
			input_mt_sync(ts->input);
			mt_sync_count++;
		} else {
			/* use 0 based track id's */
			signal = ts->platform_data->frmwrk->abs
				[(CY_ABS_ID_OST*CY_NUM_ABS_SET)+0];
			if (signal != CY_IGNORE_VALUE) {
				t = touch.abs[CY_TCH_T] -
					ts->platform_data->frmwrk->abs
					[(CY_ABS_ID_OST * CY_NUM_ABS_SET) +
					CY_MIN_OST];
#ifdef CONFIG_TOUCHSCREEN_CYPRESS_TTSP_SWAP_XY
				{
					int swap = 0;

					swap = touch.abs[CY_TCH_X];
					touch.abs[CY_TCH_X] = touch.abs[CY_TCH_Y];
					touch.abs[CY_TCH_Y] = swap;
				}
#endif
#ifdef CONFIG_TOUCHSCREEN_CYPRESS_TTSP_SWAP_XY
#ifdef CONFIG_TOUCHSCREEN_CYPRESS_TTSP_FLIP_X
				touch.abs[CY_TCH_Y] = (CY_MAXY - touch.abs[CY_TCH_Y]);
#endif
#ifdef CONFIG_TOUCHSCREEN_CYPRESS_TTSP_FLIP_Y
				touch.abs[CY_TCH_X] = (CY_MAXX - touch.abs[CY_TCH_X]);
#endif
#else
#ifdef CONFIG_TOUCHSCREEN_CYPRESS_TTSP_FLIP_X
				touch.abs[CY_TCH_X] = (CY_MAXX - touch.abs[CY_TCH_X]);
#endif
#ifdef CONFIG_TOUCHSCREEN_CYPRESS_TTSP_FLIP_Y
				touch.abs[CY_TCH_Y] = (CY_MAXY - touch.abs[CY_TCH_Y]);
#endif
#endif

				if (touch.abs[CY_TCH_E] == CY_EV_LIFTOFF) {
					/* if lift-off, then skip the touch */
					dev_dbg(ts->dev,
						"%s: t=%d e=%d lift-off\n",
						__func__, t,
						touch.abs[CY_TCH_E]);
					goto _cyttsp4_get_mt_touches_pr_tch;
				} else
					input_report_abs(ts->input, signal, t);
			}

			/* all devices: position and pressure fields */
			for (j = 0; j < CY_ABS_W_OST ; j++) {
				signal = ts->platform_data->frmwrk->abs
					[((CY_ABS_X_OST + j) *
					CY_NUM_ABS_SET) + 0];
				if (signal != CY_IGNORE_VALUE) {
					input_report_abs(ts->input, signal,
					touch.abs[CY_TCH_X + j]);
				}
			}

#ifdef CY_USE_TMA884
			/* TMA884 size field */
			signal = ts->platform_data->frmwrk->abs
				[(CY_ABS_W_OST * CY_NUM_ABS_SET) + 0];
			if (signal != CY_IGNORE_VALUE)
				input_report_abs(ts->input,
					signal, touch.abs[CY_TCH_W]);
#endif /* --CY_USE_TMA884 */

			input_mt_sync(ts->input);
			mt_sync_count++;
		}

_cyttsp4_get_mt_touches_pr_tch:
		dev_dbg(ts->dev,
         "%s: t=%d x=(%d) y=(%d) z=(%d) e=%d\n", __func__,
			t,
			touch.abs[CY_TCH_X],
			touch.abs[CY_TCH_Y],
			touch.abs[CY_TCH_P],
			touch.abs[CY_TCH_E]);

	}

	if (mt_sync_count)
		input_sync(ts->input);

	ts->num_prv_tch = num_cur_tch;

	return;
}
#include <linux/cpufreq.h>
/* read xy_data for all current touches */
static int _cyttsp4_xy_worker(struct cyttsp4 *ts)
{
	struct cyttsp4_touch touch;
	u8 num_cur_tch = 0;
	u8 hst_mode = 0;
	u8 rep_len = 0;
	u8 rep_stat = 0;
	u8 tt_stat = 0;
	int i = 0;
	int num_cur_btn = 0;
	int cur_reg = 0;
	u8 cur_btn_mask = 0;
	int cur_btn = 0;
	u8 cur_record_count= 0;

#ifdef CONFIG_TOUCHSCREEN_DEBUG
	int t = 0;
#endif /* --CONFIG_TOUCHSCREEN_DEBUG */

	enum cyttsp4_btn_state btn_state = CY_BTN_RELEASED;
	int retval = 0;

	/*
	 * Get event data from CYTTSP device.
	 * The event data includes all data
	 * for all active touches.
	 */
	/*
	 * Use 2 reads: first to get mode bytes,
	 * second to get status (touch count) and touch 1 data.
	 * An optional 3rd read to get touch 2 - touch n data.
	 */
	memset(&touch, 0, sizeof(struct cyttsp4_touch));
	memset(ts->xy_mode, 0, ts->si_ofs.mode_size);
	memset(ts->xy_data_touch1, 0, 1 + ts->si_ofs.tch_rec_siz);

	retval = _cyttsp4_load_status_regs(ts);
	if (retval < 0) {
		/*
		 * bus failure implies Watchdog -> bootloader running
		 * on TMA884 parts
		*/
		dev_err(ts->dev,
			"%s: 1st read fail on mode regs r=%d\n",
			__func__, retval);
		retval = -EIO;
		goto _cyttsp4_xy_worker_exit;
	}
	retval = _cyttsp4_read_block_data(ts, ts->si_ofs.tt_stat_ofs,
		1+ts->si_ofs.tch_rec_siz, ts->xy_data_touch1,
		ts->platform_data->addr[CY_TCH_ADDR_OFS], true);
	if (retval < 0) {
		/* bus failure may imply bootloader running */
		dev_err(ts->dev,
			"%s: read fail on mode regs r=%d\n",
			__func__, retval);
		retval = -EIO;
		goto _cyttsp4_xy_worker_exit;
	}

	hst_mode = ts->xy_mode[CY_REG_BASE];
	rep_len = ts->xy_mode[ts->si_ofs.rep_ofs];
	rep_stat = ts->xy_mode[ts->si_ofs.rep_ofs + 1];
	tt_stat = ts->xy_data_touch1[0];
	dev_dbg(ts->dev,
		"%s: hst_mode=%02X rep_len=%d rep_stat=%02X tt_stat=%02X\n",
		__func__, hst_mode, rep_len, rep_stat, tt_stat);

	if (rep_len == 0) {
		dev_err(ts->dev,
			"%s: report length error rep_len=%d\n",
			__func__, rep_len);
		goto _cyttsp4_xy_worker_exit;
	}

	if (GET_NUM_TOUCHES(tt_stat) > 0) {
		memcpy(ts->xy_data, ts->xy_data_touch1 + 1,
			ts->si_ofs.tch_rec_siz);
	}
	if (GET_NUM_TOUCHES(tt_stat) > 1) {
		retval = _cyttsp4_read_block_data(ts, ts->si_ofs.tt_stat_ofs +
			1 + ts->si_ofs.tch_rec_siz,
			(GET_NUM_TOUCHES(tt_stat) - 1) * ts->si_ofs.tch_rec_siz,
			ts->xy_data + ts->si_ofs.tch_rec_siz,
			ts->platform_data->addr[CY_TCH_ADDR_OFS], true);
		if (retval < 0) {
			dev_err(ts->dev,
			"%s: read fail on touch regs r=%d\n",
				__func__, retval);
			goto _cyttsp4_xy_worker_exit;
		}
	}
#ifdef CONFIG_TOUCHSCREEN_DEBUG
	if (ts->si_ofs.num_btns > 0) {
			retval = _cyttsp4_read_block_data(ts,
						(ts->si_ofs.tt_stat_ofs + 1) +
						(ts->si_ofs.max_tchs * ts->si_ofs.tch_rec_siz),
						ts->si_ofs.btn_rec_siz * ts->si_ofs.num_btns,
						ts->btn_rec_data,
						ts->platform_data->addr[CY_TCH_ADDR_OFS], true);
			if (retval < 0) {
					dev_err(ts->dev,
					"%s: read fail on button records r=%d\n",
							__func__, retval);
					goto _cyttsp4_xy_worker_exit;
			}
			_cyttsp4_pr_buf(ts, ts->btn_rec_data, ts->si_ofs.btn_rec_siz *
						ts->si_ofs.num_btns, "btn_rec_data");
	}
#endif /* --CONFIG_TOUCHSCREEN_Derror:EBUG */


	/* provide flow control handshake */
	retval = _cyttsp4_handshake(ts, hst_mode);
	if (retval < 0) {
		dev_err(ts->dev,
			"%s: handshake fail on operational reg\n",
			__func__);
		/* continue; rely on handshake tmo */
		retval = 0;
	}

	/* determine number of currently active touches */
	num_cur_tch = GET_NUM_TOUCHES(tt_stat);

	cur_record_count = GET_RECORD_COUNT(rep_stat);

	/* print xy data */
	_cyttsp4_pr_buf(ts, ts->xy_data, num_cur_tch *
		ts->si_ofs.tch_rec_siz, "xy_data");

	/* check for any error conditions */
	if (ts->driver_state == CY_IDLE_STATE) {
		dev_err(ts->dev,
			"%s: IDLE STATE detected\n", __func__);
		retval = 0;
		goto _cyttsp4_xy_worker_exit;
	} else if (IS_BAD_PKT(rep_stat)) {
		dev_err(ts->dev,
			"%s: Invalid buffer detected,"
			"hst_mode=%02X rep_len=%d rep_stat=%02X tt_stat=%02X\n",
			__func__, hst_mode, rep_len, rep_stat, tt_stat);
		retval = 0;
		goto _cyttsp4_xy_worker_exit;

// HASH: Removing this check due to missed 2nd-touch events.  Needs research.
#if 0
	} else if (cur_record_count == ts->prev_record_count) {
		dev_vdbg(ts->dev,
			"%s: Duplicate record count=%02X; ignore\n",
			__func__, cur_record_count);
		retval = 0;
		goto _cyttsp4_xy_worker_exit;
#endif
	} else if (IS_BOOTLOADERMODE(rep_stat)) {
		dev_info(ts->dev,
			"%s: BL mode found in ACTIVE state\n",
			__func__);
		retval = -EIO;
		goto _cyttsp4_xy_worker_exit;
	} else if (GET_HSTMODE(hst_mode) == GET_HSTMODE(CY_SYSINFO_MODE)) {
		/* if in sysinfo mode switch to op mode */
		dev_err(ts->dev,
			"%s: Sysinfo mode=0x%02X detected in ACTIVE state\n",
			__func__, hst_mode);
		retval = _cyttsp4_set_mode(ts, CY_OPERATE_MODE);
		if (retval < 0) {
			_cyttsp4_change_state(ts, CY_IDLE_STATE);
			dev_err(ts->dev,
			"%s: Fail set operational mode (r=%d)\n",
				__func__, retval);
		} else {
			_cyttsp4_change_state(ts, CY_ACTIVE_STATE);
			dev_vdbg(ts->dev,
				"%s: enable handshake\n", __func__);
#ifdef CY_USE_TMA884
			retval = _cyttsp4_handshake_enable(ts);
			if (retval < 0) {
				dev_err(ts->dev,
			"%s: fail enable handshake r=%d",
					__func__, retval);
			}
#endif /* --CY_USE_TMA884 */
		}
		goto _cyttsp4_xy_worker_exit;
	} else if (IS_LARGE_AREA(tt_stat)) {
		/* terminate all active tracks */
		num_cur_tch = 0;
		dev_dbg(ts->dev, "%s: Large area detected\n", __func__);
	} else if (num_cur_tch > ts->si_ofs.max_tchs) {
		if (num_cur_tch == 0x1F) {
			/* terminate all active tracks */
			dev_err(ts->dev,
			"%s: Num touch err detected (n=%d)\n",
				__func__, num_cur_tch);
			num_cur_tch = 0;
		} else {
			dev_err(ts->dev,
			"%s: too many tch; set to max tch (n=%d c=%d)\n",
				__func__, num_cur_tch, CY_NUM_TCH_ID);
			num_cur_tch = CY_NUM_TCH_ID;
		}
	}

	ts->prev_record_count = cur_record_count;

	dev_vdbg(ts->dev,
		"%s: num_cur_tch=%d\n", __func__, num_cur_tch);

	/* extract xy_data for all currently reported touches */
	if (num_cur_tch) {
		if (ts->num_prv_tch == 0) {
			/* ICS touch down button press signal */
			input_report_key(ts->input, BTN_TOUCH, CY_BTN_PRESSED);
		}
		_cyttsp4_get_mt_touches(ts, num_cur_tch);
	} else {
		if (ts->num_prv_tch != 0) {
			/* ICS Lift off button release signal and empty mt */
			input_report_key(ts->input, BTN_TOUCH, CY_BTN_RELEASED);
			input_mt_sync(ts->input);
			input_sync(ts->input);
		}
		ts->num_prv_tch = 0;
	}

	if (ts->si_ofs.num_btns > 0) {
		for (btn_state = CY_BTN_RELEASED; btn_state < CY_BTN_NUM_STATE;
			btn_state++) {
			for (cur_reg = 0, cur_btn = 0,
				num_cur_btn = ts->si_ofs.num_btns;
				cur_reg < ts->si_ofs.num_btn_regs;
				cur_reg++,
				cur_btn += CY_NUM_BTN_PER_REG,
				num_cur_btn -= CY_NUM_BTN_PER_REG) {
				if (num_cur_btn > 0) {
					cur_btn_mask = ts->xy_mode
						[ts->si_ofs.rep_ofs +
						2 + cur_reg];
					if (num_cur_btn / CY_NUM_BTN_PER_REG)
						i = CY_NUM_BTN_PER_REG;
					else
						i = num_cur_btn;
					switch (btn_state) {
					case CY_BTN_RELEASED:
						_cyttsp4_btn_key_release(ts,
							cur_btn,
							cur_btn_mask, i);
						break;
					case CY_BTN_PRESSED:
						_cyttsp4_btn_key_press(ts,
							cur_btn,
							cur_btn_mask, i);
						break;
					default:
						break;
					}
				}
			}
		}
#ifdef CONFIG_TOUCHSCREEN_DEBUG
		for (cur_btn = 0; cur_btn < ts->si_ofs.num_btns;
					cur_btn++) {
				ts->pr_buf[0] = 0;
					sprintf(ts->pr_buf, "btn_rec[%d]=0x", cur_btn);
						for (t = 0; t < ts->si_ofs.btn_rec_siz; t++) {
									sprintf(ts->pr_buf, "%s%02X", ts->pr_buf,
												ts->btn_rec_data[(cur_btn *
												ts->si_ofs.btn_rec_siz) + t]);
						}
						dev_dbg(ts->dev,
								"%s: %s\n", __func__, ts->pr_buf);
		}
#endif /* --CONFIG_TOUCHSCREEN_DEBUG */
	}

	dev_dbg(ts->dev,
		"%s:\n", __func__);

	retval = 0;
_cyttsp4_xy_worker_exit:
#ifdef CY_USE_LEVEL_IRQ
	udelay(500);
#endif
	return retval;
}

#ifdef CY_USE_WATCHDOG
#define CY_TIMEOUT msecs_to_jiffies(1000)
static void _cyttsp4_start_wd_timer(struct cyttsp4 *ts)
{
	mod_timer(&ts->timer, jiffies + CY_TIMEOUT);

	return;
}

static void _cyttsp4_stop_wd_timer(struct cyttsp4 *ts)
{
	del_timer(&ts->timer);
	cancel_work_sync(&ts->work);

	return;
}

static void cyttsp4_timer_watchdog(struct work_struct *work)
{
	struct cyttsp4 *ts = container_of(work, struct cyttsp4, work);
	u8 rep_stat = 0;
	int retval = 0;

	if (ts == NULL) {
		dev_err(ts->dev,
			"%s: NULL context pointer\n", __func__);
		return;
	}

	mutex_lock(&ts->data_lock);
	if (ts->driver_state == CY_ACTIVE_STATE) {
		retval = _cyttsp4_load_status_regs(ts);
		if (retval < 0) {
			dev_err(ts->dev,
			"%s: failed to access device"
				" in watchdog timer r=%d\n", __func__, retval);
			_cyttsp4_queue_startup(ts, false);
			goto cyttsp4_timer_watchdog_exit_error;
		}
		rep_stat = ts->xy_mode[ts->si_ofs.rep_ofs + 1];
		if (IS_BOOTLOADERMODE(rep_stat)) {
			dev_err(ts->dev,
			"%s: device found in bootloader mode"
				" when operational mode rep_stat=0x%02X\n",
				__func__, rep_stat);
			_cyttsp4_queue_startup(ts, false);
			goto cyttsp4_timer_watchdog_exit_error;
		}
	}

	_cyttsp4_start_wd_timer(ts);
 cyttsp4_timer_watchdog_exit_error:
	mutex_unlock(&ts->data_lock);
	return;
}

static void cyttsp4_timer(unsigned long handle)
{
	struct cyttsp4 *ts = (struct cyttsp4 *)handle;

	if (!work_pending(&ts->work))
		schedule_work(&ts->work);

	return;
}
#endif

static int _cyttsp4_soft_reset(struct cyttsp4 *ts)
{
	u8 cmd = CY_SOFT_RESET_MODE;

	return _cyttsp4_write_block_data(ts, CY_REG_BASE,
		sizeof(cmd), &cmd,
		ts->platform_data->addr[CY_TCH_ADDR_OFS], true);
}

static int _cyttsp4_reset(struct cyttsp4 *ts)
{
	enum cyttsp4_driver_state tmp_state = ts->driver_state;
	int retval = 0;

	if (ts->platform_data->hw_reset) {
		retval = ts->platform_data->hw_reset();
		if (retval == -ENOSYS) {
			retval = _cyttsp4_soft_reset(ts);
			ts->soft_reset_asserted = true;
		} else
			ts->soft_reset_asserted = false;
	} else {
		retval = _cyttsp4_soft_reset(ts);
		ts->soft_reset_asserted = true;
	}

	if (retval < 0) {
		_cyttsp4_pr_state(ts);
		return retval;
	} else {
		ts->current_mode = CY_MODE_BOOTLOADER;
		ts->driver_state = CY_BL_STATE;
		if (tmp_state != CY_BL_STATE)
			_cyttsp4_pr_state(ts);
		return retval;
	}
}

static void cyttsp4_ts_work_func(struct work_struct *work)
{
	struct cyttsp4 *ts =
		container_of(work, struct cyttsp4, cyttsp4_resume_startup_work);
	int retval = 0;

	mutex_lock(&ts->data_lock);

	input_mt_sync(ts->input);
	input_sync(ts->input);
	retval = _cyttsp4_startup(ts);
	if (retval < 0) {
		dev_err(ts->dev,
			"%s: Startup failed with error code %d\n",
			__func__, retval);
		_cyttsp4_change_state(ts, CY_IDLE_STATE);
#ifdef CY_USE_WATCHDOG
	} else {
		_cyttsp4_start_wd_timer(ts);
#endif
	}

	mutex_unlock(&ts->data_lock);

	return;
}

static int _cyttsp4_enter_sleep(struct cyttsp4 *ts)
{
	int retval = 0;
#if defined(CONFIG_PM_SLEEP) || \
	defined(CONFIG_PM) || \
	defined(CONFIG_HAS_EARLYSUSPEND)
	uint8_t sleep = CY_DEEP_SLEEP_MODE;

	if(!ts->suspend_in_prog)
	{
		dev_info(ts->dev,
			"%s: put the device back to sleep, get suspend_lock\n", __func__);

		if (ts->irq_enabled)
			disable_irq(ts->irq);

		mutex_lock(&ts->suspend_lock);
		mutex_lock(&ts->data_lock);
		ts->suspend_in_prog = true;

		retval = _cyttsp4_write_block_data(ts, CY_REG_BASE,
			sizeof(sleep), &sleep,
			ts->platform_data->addr[CY_TCH_ADDR_OFS], true);
		if (retval < 0) {
			dev_err(ts->dev,
				"%s: Failed to write sleep bit r=%d\n",
				__func__, retval);
			/* if suspend failed, need to re-enable interrupts to
			   allow recovery to take place */
			if (ts->irq_enabled)
				enable_irq(ts->irq);
		} else
			_cyttsp4_change_state(ts, CY_SLEEP_STATE);

		ts->suspend_in_prog = false;
		mutex_unlock(&ts->data_lock);
		mutex_unlock(&ts->suspend_lock);
		dev_info(ts->dev,
			"%s: The device is asleep, release suspend_lock\n", __func__);
	} else {
		dev_info(ts->dev,
			"%s: suspend discarded, already in progress", __func__);
	}
#endif
	return retval;
}

static int _cyttsp4_wakeup(struct cyttsp4 *ts)
{
	int retval = 0;
#if defined(CONFIG_PM_SLEEP) || \
	defined(CONFIG_PM) || \
	defined(CONFIG_HAS_EARLYSUSPEND)
	unsigned long timeout = 0;
	unsigned long uretval = 0;
	u8 hst_mode = 0;
	int wake = CY_WAKE_DFLT;

	if(!ts->resume_in_prog)
	{
		dev_info(ts->dev,"%s getting suspend_lock\n", __func__);
		mutex_lock(&ts->suspend_lock);
		mutex_lock(&ts->data_lock);
		ts->resume_in_prog = true;

		if (ts->irq_enabled) {
			enable_irq(ts->irq);
		}

		_cyttsp4_change_state(ts, CY_CMD_STATE);
		INIT_COMPLETION(ts->int_running);
		if (ts->platform_data->hw_recov == NULL) {
			dev_vdbg(ts->dev,
				"%s: no hw_recov function\n", __func__);
			retval = -ENOSYS;
		} else {
			/* wake using strobe on host alert pin */
			retval = ts->platform_data->hw_recov(wake);
			if (retval < 0) {
				if (retval == -ENOSYS) {
					dev_vdbg(ts->dev,
						"%s: no hw_recov wake code=%d"
						" function\n", __func__, wake);
				} else {
					dev_err(ts->dev,
				"%s: fail hw_recov(wake=%d)"
						" function r=%d\n",
						__func__, wake, retval);
					retval = -ENOSYS;
				}
			}
		}

		if (retval == -ENOSYS) {
			/*
			 * Wake the chip with bus traffic
			 * The first few reads should always fail because
			 * the part is not ready to respond,
			 * but the retries should succeed.
			 */
			/*
			 * Even though this is hardware-specific, it is done
			 * here because the board config file doesn't have
			 * access to the bus read routine
			 */
			retval = _cyttsp4_read_block_data(ts, CY_REG_BASE,
				sizeof(hst_mode), &hst_mode,
				ts->platform_data->addr[CY_TCH_ADDR_OFS],
				true);
			if (retval < 0) {
				/* device may not be ready even with the
				 * bus read retries so just go ahead and
				 * wait for the cmd rdy interrupt or timeout
				 */
				retval = 0;
			} else {
				/* IC is awake but still need to check for
				 * proper mode
				 */
			}
		} else
			retval = 0;

		/* Wait for cmd rdy interrupt to signal device wake */
		timeout = msecs_to_jiffies(CY_HALF_SEC_TMO_MS);
		mutex_unlock(&ts->data_lock);
		uretval = wait_for_completion_interruptible_timeout(
			&ts->int_running, timeout);
		mutex_lock(&ts->data_lock);

		/* read registers even if wait ended with timeout */
		retval = _cyttsp4_read_block_data(ts,
			CY_REG_BASE, sizeof(hst_mode), &hst_mode,
			ts->platform_data->addr[CY_TCH_ADDR_OFS], true);

		/* TMA884 indicates bootloader mode by changing addr */
		if (retval < 0) {
			dev_err(ts->dev,
				"%s: failed to resume or in bootloader (r=%d)\n",
				__func__, retval);
		} else {

			retval = _cyttsp4_handshake(ts, hst_mode);
			if (retval < 0) {
				dev_err(ts->dev,
				"%s: fail resume INT handshake (r=%d)\n",
					__func__, retval);
				/* continue; rely on handshake tmo */
				retval = 0;
			}
			_cyttsp4_change_state(ts, CY_ACTIVE_STATE);
		}
		ts->resume_in_prog = false;
		mutex_unlock(&ts->data_lock);
		mutex_unlock(&ts->suspend_lock);
		dev_info(ts->dev,"%s suspend_lock_released\n", __func__);
	} else {
		dev_info(ts->dev,"%s: resume discarded, already in progress\n", __func__);
	}
#endif
	return retval;
}

#if defined(CONFIG_PM) || \
	defined(CONFIG_PM_SLEEP) || \
	defined(CONFIG_HAS_EARLYSUSPEND)

#if defined(CONFIG_HAS_EARLYSUSPEND)
int cyttsp4_suspend(void *handle)
{
	struct cyttsp4 *ts = handle;
#elif defined(CONFIG_PM_SLEEP)
static int cyttsp4_suspend(struct device *dev)
{
	struct cyttsp4 *ts = dev_get_drvdata(dev);
#else
int cyttsp4_suspend(void *handle)
{
	struct cyttsp4 *ts = handle;
#endif
	int retval = 0;

	if (ts->test.cur_mode != CY_TEST_MODE_NORMAL_OP) {
		retval = -EBUSY;
		dev_err(ts->dev,
			"%s: Suspend Blocked while in test mode=%d\n",
			__func__, ts->test.cur_mode);
	} else {
		switch (ts->driver_state) {
		case CY_ACTIVE_STATE:
#if defined(CY_USE_FORCE_LOAD) || defined(CONFIG_TOUCHSCREEN_DEBUG)
			if (ts->waiting_for_fw) {
				retval = -EBUSY;
				dev_err(ts->dev,
			"%s: Suspend Blocked while waiting for"
					" fw load in %s state\n", __func__,
					cyttsp4_driver_state_string
					[ts->driver_state]);
				break;
			}
#endif

			dev_vdbg(ts->dev,
				"%s: Suspending...\n", __func__);

#ifdef CY_USE_WATCHDOG
			_cyttsp4_stop_wd_timer(ts);
#endif

			retval = _cyttsp4_enter_sleep(ts);
			if (retval < 0) {
				dev_err(ts->dev,
			"%s: fail enter sleep r=%d\n",
					__func__, retval);
			} else
				_cyttsp4_change_state(ts, CY_SLEEP_STATE);

			ts->suspend_blocked = false;
			break;
		case CY_SLEEP_STATE:
			dev_err(ts->dev,
			"%s: already in Sleep state\n", __func__);
			break;
		/*
		 * These states could be changing the device state
		 * Some of these states don't directly change device state
		 * but the next state could happen at any time and that
		 * state DOES modify the device state
		 * they must complete before allowing suspend.
		 */
		case CY_BL_STATE:
		case CY_CMD_STATE:
		case CY_OPCMD_STATE:
		case CY_SYSINFO_STATE:
		case CY_READY_STATE:
		case CY_TRANSFER_STATE:
			dev_vdbg(ts->dev,
			"%s: Suspend Blocked while in %s state\n",
				__func__, cyttsp4_driver_state_string
				[ts->driver_state]);
			ts->suspend_blocked = true;
			break;
		case CY_IDLE_STATE:
		case CY_INVALID_STATE:
		default:
			dev_err(ts->dev,
			"%s: Cannot enter suspend from %s state\n",
				__func__, cyttsp4_driver_state_string
				[ts->driver_state]);
			break;
		}
	}

	return retval;
}
EXPORT_SYMBOL_GPL(cyttsp4_suspend);

#if defined(CONFIG_HAS_EARLYSUSPEND)
int cyttsp4_resume(void *handle)
{
	struct cyttsp4 *ts = handle;
#elif defined(CONFIG_PM_SLEEP)
static int cyttsp4_resume(struct device *dev)
{
	struct cyttsp4 *ts = dev_get_drvdata(dev);
#else
int cyttsp4_resume(void *handle)
{
	struct cyttsp4 *ts = handle;
#endif
	int retval = 0;

	dev_dbg(ts->dev, "%s: Resuming...\n", __func__);

#ifdef CY_USE_LEVEL_IRQ
	/* workaround level interrupt unmasking issue */
	if (ts->irq_enabled) {
		disable_irq_nosync(ts->irq);
		udelay(5);
		enable_irq(ts->irq);
	}
#endif

	switch (ts->driver_state) {
	case CY_SLEEP_STATE:

		retval = _cyttsp4_wakeup(ts);
		if (retval < 0) {
			dev_err(ts->dev,
			"%s: wakeup fail r=%d\n",
				__func__, retval);
			_cyttsp4_pr_state(ts);

			_cyttsp4_queue_startup(ts, false);
			break;
		}
		_cyttsp4_change_state(ts, CY_ACTIVE_STATE);


#ifdef CY_USE_WATCHDOG
		_cyttsp4_start_wd_timer(ts);
#endif
		break;
	case CY_IDLE_STATE:
	case CY_READY_STATE:
	case CY_ACTIVE_STATE:
	case CY_BL_STATE:
	case CY_SYSINFO_STATE:
	case CY_CMD_STATE:
	case CY_TRANSFER_STATE:
	case CY_INVALID_STATE:
	default:
		dev_err(ts->dev,
			"%s: Already in %s state\n", __func__,
			cyttsp4_driver_state_string[ts->driver_state]);
		break;
	}

	/* Check to see if charger/hdmi state was updated
	 * while IC was in sleep mode
	 */
	if (ts->charger_hdmi_update_pending == true)
	{

		dev_vdbg(ts->dev, "%s:calling write_charger_hdmi\n", __func__);
		msleep(CY_DELAY_DFLT);
		write_charger_hdmi_config(ts, ts->charger_hdmi);
		ts->charger_hdmi_update_pending = false;
	}

	dev_vdbg(ts->dev,
		"%s: exit Resume r=%d\n", __func__, retval);

	return retval;
}
EXPORT_SYMBOL_GPL(cyttsp4_resume);
#endif
#if !defined(CONFIG_HAS_EARLYSUSPEND) && defined(CONFIG_PM_SLEEP)
const struct dev_pm_ops cyttsp4_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(cyttsp4_suspend, cyttsp4_resume)
};
EXPORT_SYMBOL_GPL(cyttsp4_pm_ops);
#endif


#if defined(CONFIG_HAS_EARLYSUSPEND)
void cyttsp4_early_suspend(struct early_suspend *h)
{
	struct cyttsp4 *ts = container_of(h, struct cyttsp4, early_suspend);
	int retval = 0;

	dev_vdbg(ts->dev, "%s: EARLY SUSPEND ts=%p\n",
		__func__, ts);
	retval = cyttsp4_suspend(ts);
	if (retval < 0) {
		dev_err(ts->dev,
			"%s: Early suspend failed with error code %d\n",
			__func__, retval);
	}
}
void cyttsp4_late_resume(struct early_suspend *h)
{
	struct cyttsp4 *ts = container_of(h, struct cyttsp4, early_suspend);
	int retval = 0;

	dev_vdbg(ts->dev, "%s: LATE RESUME ts=%p\n",
		__func__, ts);
	retval = cyttsp4_resume(ts);
	if (retval < 0) {
		dev_err(ts->dev,
			"%s: Late resume failed with error code %d\n",
			__func__, retval);
	}
}
#endif

#ifdef CY_AUTO_LOAD_FW
static int _cyttsp4_boot_loader(struct cyttsp4 *ts, bool *upgraded)
{
	int retval = 0;
	int i = 0;
	u32 fw_vers_platform = 0;
	u32 fw_vers_img = 0;
	u32 fw_revctrl_platform_h = 0;
	u32 fw_revctrl_platform_l = 0;
	u32 fw_revctrl_img_h = 0;
	u32 fw_revctrl_img_l = 0;
	bool new_fw_vers = false;
	bool new_fw_revctrl = false;
	bool new_vers = false;

	*upgraded = false;
	if (ts->driver_state == CY_SLEEP_STATE) {
		dev_err(ts->dev,
			"%s: cannot load firmware in sleep state\n",
			__func__);
		retval = 0;
	} else if ((ts->platform_data->fw->ver == NULL) ||
		(ts->platform_data->fw->img == NULL)) {
		dev_err(ts->dev,
			"%s: empty version list or no image\n",
			__func__);
		retval = 0;
	} else if (ts->platform_data->fw->vsize != CY_BL_VERS_SIZE) {
		dev_err(ts->dev,
			"%s: bad fw version list size=%d\n",
			__func__, ts->platform_data->fw->vsize);
		retval = 0;
	} else {
		/* automatically update firmware if new version detected */
		fw_vers_img = (ts->sysinfo_ptr.cydata->fw_ver_major * 256);
		fw_vers_img += ts->sysinfo_ptr.cydata->fw_ver_minor;
		fw_vers_platform = ts->platform_data->fw->ver[2] * 256;
		fw_vers_platform += ts->platform_data->fw->ver[3];
#ifdef CY_ANY_DIFF_NEW_VER_MM
		if (fw_vers_platform != fw_vers_img)
			new_fw_vers = true;
		else
			new_fw_vers = false;
#else
		if (fw_vers_platform > fw_vers_img)
			new_fw_vers = true;
		else
			new_fw_vers = false;
#endif
		dev_vdbg(ts->dev,
			"%s: fw_vers_platform=%04X fw_vers_img=%04X\n",
			__func__, fw_vers_platform, fw_vers_img);

		fw_revctrl_img_h = ts->sysinfo_ptr.cydata->revctrl[0];
		fw_revctrl_img_l = ts->sysinfo_ptr.cydata->revctrl[4];
		fw_revctrl_platform_h = ts->platform_data->fw->ver[4];
		fw_revctrl_platform_l = ts->platform_data->fw->ver[8];
		for (i = 1; i < 4; i++) {
			fw_revctrl_img_h = (fw_revctrl_img_h * 256) +
				ts->sysinfo_ptr.cydata->revctrl[0+i];
			fw_revctrl_img_l = (fw_revctrl_img_l * 256) +
				ts->sysinfo_ptr.cydata->revctrl[4+i];
			fw_revctrl_platform_h = (fw_revctrl_platform_h * 256) +
				ts->platform_data->fw->ver[4+i];
			fw_revctrl_platform_l = (fw_revctrl_platform_l * 256) +
				ts->platform_data->fw->ver[8+i];
		}
#ifdef CY_ANY_DIFF_NEW_VER
		if (fw_revctrl_platform_h != fw_revctrl_img_h)
			new_fw_revctrl = true;
		else if (fw_revctrl_platform_h == fw_revctrl_img_h) {
			if (fw_revctrl_platform_l != fw_revctrl_img_l)
				new_fw_revctrl = true;
			else
				new_fw_revctrl = false;
		} else
			new_fw_revctrl = false;
#else
		if (fw_revctrl_platform_h > fw_revctrl_img_h)
			new_fw_revctrl = true;
		else if (fw_revctrl_platform_h == fw_revctrl_img_h) {
			if (fw_revctrl_platform_l > fw_revctrl_img_l)
				new_fw_revctrl = true;
			else
				new_fw_revctrl = false;
		} else
			new_fw_revctrl = false;
#endif
		if (new_fw_vers || new_fw_revctrl)
			new_vers = true;

		pr_info(
			"%s: fw_revctrl_platform_h=%08X"
			" fw_revctrl_img_h=%08X\n", __func__,
			fw_revctrl_platform_h, fw_revctrl_img_h);
		pr_info(
			"%s: fw_revctrl_platform_l=%08X"
			" fw_revctrl_img_l=%08X\n", __func__,
			fw_revctrl_platform_l, fw_revctrl_img_l);
		pr_info(
			"%s: new_fw_vers=%d new_fw_revctrl=%d new_vers=%d\n",
			__func__,
			(int)new_fw_vers, (int)new_fw_revctrl, (int)new_vers);

		if (new_vers) {
			dev_info(ts->dev,
			"%s: upgrading firmware...\n", __func__);
			retval = _cyttsp4_load_app(ts,
				ts->platform_data->fw->img,
				ts->platform_data->fw->size);
			if (retval < 0) {
				dev_err(ts->dev,
			"%s: communication fail"
					" on load fw r=%d\n",
					__func__, retval);
				_cyttsp4_change_state(ts, CY_IDLE_STATE);
				retval = -EIO;
			} else
				*upgraded = true;
		} else {
			dev_vdbg(ts->dev,
				"%s: No auto firmware upgrade required\n",
				__func__);
		}
	}

	return retval;
}
#endif /* --CY_AUTO_LOAD_FW */

static ssize_t cyttsp4_ic_ver_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct cyttsp4 *ts = dev_get_drvdata(dev);

	return sprintf(buf, "%s: 0x%02X 0x%02X\n%s: 0x%02X\n%s: 0x%02X\n%s: "
		"0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X\n",
		"TrueTouch Product ID",
		ts->sysinfo_ptr.cydata->ttpidh,
		ts->sysinfo_ptr.cydata->ttpidl,
		"Firmware Major Version", ts->sysinfo_ptr.cydata->fw_ver_major,
		"Firmware Minor Version", ts->sysinfo_ptr.cydata->fw_ver_minor,
		"Revision Control Number", ts->sysinfo_ptr.cydata->revctrl[0],
		ts->sysinfo_ptr.cydata->revctrl[1],
		ts->sysinfo_ptr.cydata->revctrl[2],
		ts->sysinfo_ptr.cydata->revctrl[3],
		ts->sysinfo_ptr.cydata->revctrl[4],
		ts->sysinfo_ptr.cydata->revctrl[5],
		ts->sysinfo_ptr.cydata->revctrl[6],
		ts->sysinfo_ptr.cydata->revctrl[7]);
}
static DEVICE_ATTR(ic_ver, S_IRUGO, cyttsp4_ic_ver_show, NULL);

static ssize_t cyttsp4_ic_ver_raw_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct cyttsp4 *ts = dev_get_drvdata(dev);

	return sprintf(buf, "%02X%02X%02X%02X"
		"%02X%02X%02X%02X%02X%02X%02X%02X\n",
		ts->sysinfo_ptr.cydata->ttpidh,
		ts->sysinfo_ptr.cydata->ttpidl,
		ts->sysinfo_ptr.cydata->fw_ver_major,
		ts->sysinfo_ptr.cydata->fw_ver_minor,
		ts->sysinfo_ptr.cydata->revctrl[0],
		ts->sysinfo_ptr.cydata->revctrl[1],
		ts->sysinfo_ptr.cydata->revctrl[2],
		ts->sysinfo_ptr.cydata->revctrl[3],
		ts->sysinfo_ptr.cydata->revctrl[4],
		ts->sysinfo_ptr.cydata->revctrl[5],
		ts->sysinfo_ptr.cydata->revctrl[6],
		ts->sysinfo_ptr.cydata->revctrl[7]);
}
static DEVICE_ATTR(ic_ver_raw, S_IRUGO, cyttsp4_ic_ver_raw_show, NULL);

/* Driver version */
static ssize_t cyttsp4_drv_ver_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct cyttsp4 *ts = dev_get_drvdata(dev);

	return snprintf(buf, CY_MAX_PRBUF_SIZE,
		"Driver: %s\nVersion: %s\nDate: %s\n",
		ts->input->name, CY_DRIVER_VERSION, CY_DRIVER_DATE);
}
static DEVICE_ATTR(drv_ver, S_IRUGO, cyttsp4_drv_ver_show, NULL);


/* Driver status */
static ssize_t cyttsp4_drv_stat_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct cyttsp4 *ts = dev_get_drvdata(dev);

	return snprintf(buf, CY_MAX_PRBUF_SIZE,
		"Driver state is %s\n",
		cyttsp4_driver_state_string[ts->driver_state]);
}
static DEVICE_ATTR(drv_stat, S_IRUGO, cyttsp4_drv_stat_show, NULL);



int write_charger_hdmi_config(struct cyttsp4 *ts, u8 value)
{
	int retval = 0;
	u8 *pdata = NULL;
	u8 charger_enabled = 0;
	bool lpe_local = ts->low_power_enable;
	u8 cmd_dat[CY_NUM_DAT + 1];	/* +1 for cmd byte */

	ts->low_power_enable = false;

	dev_info(
		ts->dev, "%s:getting suspend_lock\n", __func__);

	mutex_lock(&ts->suspend_lock);
	mutex_lock(&ts->data_lock);
	if (value == CY_CHARGER_ONLY) {
		dev_vdbg(ts->dev,
			"%s: charger only\n", __func__);
		charger_enabled = 1;
	} else if (value == CY_HDMI_ONLY) {
		dev_vdbg(ts->dev,
			"%s: hdmi only\n", __func__);
		charger_enabled = 1;
	} else if (value == CY_CHARGER_HDMI) {
		dev_vdbg(ts->dev,
			"%s: charger + hdmi\n", __func__);
		charger_enabled = 1;
	} else if (value == CY_NONE) {
		dev_vdbg(ts->dev,
			"%s: none\n", __func__);
		charger_enabled = 0;
	} else {
		dev_err(ts->dev,
			"%s: value=%u should be between 0 and 3"
			" charger_hdmi status=0x%04X\n", __func__, value, ts->flags);
		goto cyttsp4_write_charger_hdmi_config_exit;
	}

	memset(cmd_dat, 0, sizeof(cmd_dat));
	cmd_dat[0] = CY_SET_CHRGHDMI_BIT; /* populate Set Charger/HDMI command*/;
	cmd_dat[1] = charger_enabled; /* populate charger state*/

	retval = _cyttsp4_put_cmd_wait(ts, ts->si_ofs.cmd_ofs,
		sizeof(cmd_dat), cmd_dat, CY_HALF_SEC_TMO_MS,
		_cyttsp4_chk_cmd_rdy, NULL,
		ts->platform_data->addr[CY_TCH_ADDR_OFS], true, CY_OPCMD_STATE);
	if (retval < 0) {
		dev_err(ts->dev,
			"%s: Fail Set Charger/HDMI bit command r=%d\n",
			__func__, retval);
		goto cyttsp4_write_charger_hdmi_config_exit;
	}

	memset(cmd_dat, 0, sizeof(cmd_dat));
	retval = _cyttsp4_read_block_data(ts, ts->si_ofs.cmd_ofs,
		sizeof(cmd_dat), cmd_dat,
		ts->platform_data->addr[CY_TCH_ADDR_OFS], true);
	if (retval < 0) {
		dev_err(ts->dev,
			"%s: Fail Set Charger/HDMI status r=%d\n",
			__func__, retval);
		goto cyttsp4_write_charger_hdmi_config_exit;
	}

	/* Check return value */
	if (cmd_dat[1] != 0) {
		dev_err(ts->dev,
			"%s: Fail Set Charger/HDMI %d status=%d %d %d %d error\n",
			__func__, cmd_dat[0], cmd_dat[1], cmd_dat[2], cmd_dat[3], cmd_dat[4]);
		retval = -EIO;
		goto cyttsp4_write_charger_hdmi_config_exit;
	}

	retval = _cyttsp4_cmd_handshake(ts);
	if (retval < 0) {
		dev_err(ts->dev,
		"%s: Command handshake error r=%d\n",
			__func__, retval);
		retval = -EIO;
	}

cyttsp4_write_charger_hdmi_config_exit:
    mutex_unlock(&ts->data_lock);
	mutex_unlock(&ts->suspend_lock);

	dev_info(
		ts->dev, "%s: suspend_lock released\n", __func__);

	/* suspend was blocked due to execution of
	 * charger_hdmi update, suspend now
	 */
	if (ts->suspend_blocked == true)
	{
		dev_vdbg(
			ts->dev, "%s:suspending from write_charger_hdmi\n", __func__);


#ifdef CY_USE_WATCHDOG
		_cyttsp4_stop_wd_timer(ts);
#endif

		retval = _cyttsp4_enter_sleep(ts);
		if (retval < 0) {
			dev_err(ts->dev,
		"%s: fail enter sleep r=%d\n",
				__func__, retval);
		} else
			_cyttsp4_change_state(ts, CY_SLEEP_STATE);

		ts->suspend_blocked = false;
	}

	ts->low_power_enable = lpe_local;

	kfree(pdata);
	return retval;
}

static ssize_t cyttsp4_charger_hdmi_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct cyttsp4 *ts = dev_get_drvdata(dev);

	return snprintf(buf, CY_MAX_PRBUF_SIZE,
		"Charger/HDMI status: 0x%04X\n", ts->charger_hdmi);
}
static ssize_t cyttsp4_charger_hdmi_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	struct cyttsp4 *ts = dev_get_drvdata(dev);
	unsigned long value = 0;
	ssize_t retval = 0;

	retval = strict_strtoul(buf, 8, &value);
	if (retval < 0) {
		dev_err(ts->dev,
			"%s: Failed to convert value\n", __func__);
		goto cyttsp4_charger_hdmi_store_error_exit;
	}

	if (ts->charger_hdmi != value) {

		if (ts->driver_state != CY_SLEEP_STATE) {
			write_charger_hdmi_config(ts, value);
		} else {
			ts->charger_hdmi_update_pending = true;
		}
		ts->charger_hdmi = value;
	}

	dev_vdbg(ts->dev,
		"%s: Charger/HDMI status=0x%04X\n", __func__, ts->charger_hdmi);

cyttsp4_charger_hdmi_store_error_exit:
	retval = size;
	return retval;
}
static DEVICE_ATTR(charger_hdmi, S_IRWXU | S_IRWXG | S_IRWXO,
	cyttsp4_charger_hdmi_show, cyttsp4_charger_hdmi_store);


#ifdef CY_USE_REG_ACCESS
static ssize_t cyttsp_drv_rw_regid_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct cyttsp4 *ts = dev_get_drvdata(dev);

	return snprintf(buf, CY_MAX_PRBUF_SIZE,
		"Current Read/Write Regid=%02X(%d)\n",
		ts->rw_regid, ts->rw_regid);
}
static ssize_t cyttsp_drv_rw_regid_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t size)
{
	struct cyttsp4 *ts = dev_get_drvdata(dev);
	int retval = 0;
	unsigned long value;

	mutex_lock(&ts->data_lock);
	retval = strict_strtoul(buf, 10, &value);
	if (retval < 0) {
		retval = strict_strtoul(buf, 16, &value);
		if (retval < 0) {
			dev_err(ts->dev,
			"%s: Failed to convert value\n",
				__func__);
			goto cyttsp_drv_rw_regid_store_exit;
		}
	}

	if (value > CY_RW_REGID_MAX) {
		ts->rw_regid = CY_RW_REGID_MAX;
		dev_err(ts->dev,
			"%s: Invalid Read/Write Regid; set to max=%d\n",
			__func__, ts->rw_regid);
	} else
		ts->rw_regid = value;

	retval = size;

cyttsp_drv_rw_regid_store_exit:
	mutex_unlock(&ts->data_lock);
	return retval;
}
static DEVICE_ATTR(drv_rw_regid, S_IWUSR | S_IRUGO,
	cyttsp_drv_rw_regid_show, cyttsp_drv_rw_regid_store);

static ssize_t cyttsp_drv_rw_reg_data_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct cyttsp4 *ts = dev_get_drvdata(dev);
	int retval;
	u8 reg_data;

	retval = _cyttsp4_read_block_data(ts, ts->rw_regid,
			sizeof(reg_data), &reg_data,
			ts->platform_data->addr[CY_TCH_ADDR_OFS], true);

	if (retval < 0)
		return snprintf(buf, CY_MAX_PRBUF_SIZE,
			"Read/Write Regid(%02X(%d) Failed\n",
			ts->rw_regid, ts->rw_regid);
	else
		return snprintf(buf, CY_MAX_PRBUF_SIZE,
			"Read/Write Regid=%02X(%d) Data=%02X(%d)\n",
			ts->rw_regid, ts->rw_regid, reg_data, reg_data);
}
static ssize_t cyttsp_drv_rw_reg_data_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t size)
{
	struct cyttsp4 *ts = dev_get_drvdata(dev);
	int retval = 0;
	unsigned long value;
	u8 reg_data = 0;

	retval = strict_strtoul(buf, 10, &value);
	if (retval < 0) {
		retval = strict_strtoul(buf, 16, &value);
		if (retval < 0) {
			dev_err(ts->dev,
			"%s: Failed to convert value\n",
				__func__);
			goto cyttsp_drv_rw_reg_data_store_exit;
		}
	}

	if (value > CY_RW_REG_DATA_MAX) {
		dev_err(ts->dev,
			"%s: Invalid Register Data Range; no write\n",
			__func__);
	} else {
		reg_data = (u8)value;
		retval = _cyttsp4_write_block_data(ts, ts->rw_regid,
			sizeof(reg_data), &reg_data,
			ts->platform_data->addr[CY_TCH_ADDR_OFS], true);
		if (retval < 0) {
			dev_err(ts->dev,
			"%s: Failed write to Regid=%02X(%d)\n",
				__func__, ts->rw_regid, ts->rw_regid);
		}
	}

	retval = size;

cyttsp_drv_rw_reg_data_store_exit:
	return retval;
}
static DEVICE_ATTR(drv_rw_reg_data, S_IWUSR | S_IRUGO,
	cyttsp_drv_rw_reg_data_show, cyttsp_drv_rw_reg_data_store);
#endif

#ifdef CONFIG_TOUCHSCREEN_DEBUG
/* Group Number */
static ssize_t cyttsp4_ic_grpnum_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct cyttsp4 *ts = dev_get_drvdata(dev);

	return snprintf(buf, CY_MAX_PRBUF_SIZE,
		"Current Group: %d\n", ts->ic_grpnum);
}
static ssize_t cyttsp4_ic_grpnum_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	struct cyttsp4 *ts = dev_get_drvdata(dev);
	unsigned long value = 0;
	int retval = 0;

	mutex_lock(&(ts->data_lock));
	retval = strict_strtoul(buf, 10, &value);
	if (retval < 0) {
		dev_err(ts->dev,
			"%s: Failed to convert value\n", __func__);
		goto cyttsp4_ic_grpnum_store_error_exit;
	}

	if (value > 0xFF) {
		value = 0xFF;
		dev_err(ts->dev,
			"%s: value is greater than max;"
			" set to %d\n", __func__, (int)value);
	}
	ts->ic_grpnum = value;

	dev_vdbg(ts->dev,
		"%s: grpnum=%d\n", __func__, ts->ic_grpnum);

cyttsp4_ic_grpnum_store_error_exit:
	retval = size;
	mutex_unlock(&(ts->data_lock));
	return retval;
}
static DEVICE_ATTR(ic_grpnum, S_IRWXU | S_IRWXG | S_IRWXO,
	cyttsp4_ic_grpnum_show, cyttsp4_ic_grpnum_store);

/* Group Offset */
static ssize_t cyttsp4_ic_grpoffset_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct cyttsp4 *ts = dev_get_drvdata(dev);

	return snprintf(buf, CY_MAX_PRBUF_SIZE,
		"Current Offset: %u\n", ts->ic_grpoffset);
}
static ssize_t cyttsp4_ic_grpoffset_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t size)
{
	struct cyttsp4 *ts = dev_get_drvdata(dev);
	unsigned long value;
	int retval = 0;

	mutex_lock(&(ts->data_lock));
	retval = strict_strtoul(buf, 10, &value);
	if (retval < 0) {
		dev_err(ts->dev,
			"%s: Failed to convert value\n", __func__);
		goto cyttsp4_ic_grpoffset_store_error_exit;
	}

#ifdef CY_USE_TMA884
	if (value > 0xFF) {
		value = 0xFF;
		dev_err(ts->dev,
			"%s: value is greater than max;"
			" set to %d\n", __func__, (int)value);
	}
#endif /* --CY_USE_TMA884 */
	ts->ic_grpoffset = value;

	dev_vdbg(ts->dev,
		"%s: grpoffset=%d\n", __func__, ts->ic_grpoffset);

cyttsp4_ic_grpoffset_store_error_exit:
	retval = size;
	mutex_unlock(&(ts->data_lock));
	return retval;
}
static DEVICE_ATTR(ic_grpoffset, S_IRWXU | S_IRWXG | S_IRWXO,
	cyttsp4_ic_grpoffset_show, cyttsp4_ic_grpoffset_store);


static ssize_t _cyttsp4_ic_grpdata_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct cyttsp4 *ts = dev_get_drvdata(dev);
	int i;
	int retval = 0;
	size_t num_read = 0;
	u8 *ic_buf;
	u8 *pdata;
#ifdef CY_USE_TMA884
	size_t ndata = 0;
	u8 blockid = 0;
#endif /* --CY_USE_TMA884 */

	ic_buf = kzalloc(CY_MAX_PRBUF_SIZE, GFP_KERNEL);
	if (ic_buf == NULL) {
		dev_err(ts->dev,
			"%s: Failed to allocate buffer for %s\n",
			__func__, "ic_grpdata_show");
		return snprintf(buf, CY_MAX_PRBUF_SIZE,
			"Group %d buffer allocation error.\n",
			ts->ic_grpnum);
	}
	dev_vdbg(ts->dev,
		"%s: grpnum=%d grpoffset=%u\n",
		__func__, ts->ic_grpnum, ts->ic_grpoffset);

	if (ts->ic_grpnum >= CY_IC_GRPNUM_NUM) {
		dev_err(ts->dev,
			"%s: Group %d does not exist.\n",
			__func__, ts->ic_grpnum);
		kfree(ic_buf);
		return snprintf(buf, CY_MAX_PRBUF_SIZE,
			"Group %d does not exist.\n",
			ts->ic_grpnum);
	}

	switch (ts->ic_grpnum) {
	case CY_IC_GRPNUM_RESERVED:
		goto cyttsp4_ic_grpdata_show_grperr;
		break;
	case CY_IC_GRPNUM_CMD_REGS:
		num_read = ts->si_ofs.rep_ofs - ts->si_ofs.cmd_ofs;
		dev_vdbg(ts->dev,
			"%s: GRP=CMD_REGS: num_read=%d at ofs=%d + grpofs=%d\n",
			__func__, num_read,
			ts->si_ofs.cmd_ofs, ts->ic_grpoffset);
		if (ts->ic_grpoffset >= num_read)
			goto cyttsp4_ic_grpdata_show_ofserr;
		else {
			num_read -= ts->ic_grpoffset;
			retval = _cyttsp4_read_block_data(ts, ts->ic_grpoffset +
				ts->si_ofs.cmd_ofs, num_read, ic_buf,
				ts->platform_data->addr[CY_TCH_ADDR_OFS], true);
			if (retval < 0)
				goto cyttsp4_ic_grpdata_show_prerr;
		}
		break;
	case CY_IC_GRPNUM_TCH_REP:
		num_read = ts->si_ofs.rep_sz;
		dev_vdbg(ts->dev,
			"%s: GRP=TCH_REP: num_read=%d at ofs=%d + grpofs=%d\n",
			__func__, num_read,
			ts->si_ofs.rep_ofs, ts->ic_grpoffset);
		if (ts->ic_grpoffset >= num_read)
			goto cyttsp4_ic_grpdata_show_ofserr;
		else {
			num_read -= ts->ic_grpoffset;
			retval = _cyttsp4_read_block_data(ts, ts->ic_grpoffset +
				ts->si_ofs.rep_ofs, num_read, ic_buf,
				ts->platform_data->addr[CY_TCH_ADDR_OFS], true);
			if (retval < 0)
				goto cyttsp4_ic_grpdata_show_prerr;
		}
		break;
	case CY_IC_GRPNUM_DATA_REC:
		num_read = ts->si_ofs.cydata_size;
		dev_vdbg(ts->dev,
			"%s: GRP=DATA_REC: num_read=%d at ofs=%d + grpofs=%d\n",
			__func__, num_read,
			ts->si_ofs.cydata_ofs, ts->ic_grpoffset);
		if (ts->ic_grpoffset >= num_read)
			goto cyttsp4_ic_grpdata_show_ofserr;
		else {
			num_read -= ts->ic_grpoffset;
			retval = _cyttsp4_set_mode(ts, CY_SYSINFO_MODE);
			if (retval < 0) {
				dev_err(ts->dev,
			"%s: Fail enter Sysinfo mode r=%d\n",
					__func__, retval);
				goto cyttsp4_ic_grpdata_show_data_rderr;
			}
			retval = _cyttsp4_read_block_data(ts, ts->ic_grpoffset +
				ts->si_ofs.cydata_ofs, num_read, ic_buf,
				ts->platform_data->addr[CY_TCH_ADDR_OFS], true);
			if (retval < 0) {
				dev_err(ts->dev,
			"%s: Fail read Sysinfo ddata r=%d\n",
					__func__, retval);
				goto cyttsp4_ic_grpdata_show_data_rderr;
			}
			retval = _cyttsp4_set_mode(ts, CY_OPERATE_MODE);
			if (retval < 0) {
				dev_err(ts->dev,
			"%s: Fail enter Operational mode r=%d\n",
					__func__, retval);
			}
		}
		break;
cyttsp4_ic_grpdata_show_data_rderr:
		dev_err(ts->dev,
			"%s: Fail read cydata record\n", __func__);
		goto cyttsp4_ic_grpdata_show_prerr;
		break;
	case CY_IC_GRPNUM_TEST_REC:
		num_read = ts->si_ofs.test_size;
	    dev_vdbg(ts->dev,
			"%s: GRP=TEST_REC: num_read=%d at ofs=%d + grpofs=%d\n",
			__func__, num_read,
			ts->si_ofs.test_ofs, ts->ic_grpoffset);
		if (ts->ic_grpoffset >= num_read)
			goto cyttsp4_ic_grpdata_show_ofserr;
		else {
			num_read -= ts->ic_grpoffset;
			retval = _cyttsp4_set_mode(ts, CY_SYSINFO_MODE);
			if (retval < 0) {
				dev_err(ts->dev,
			"%s: Fail enter Sysinfo mode r=%d\n",
					__func__, retval);
				goto cyttsp4_ic_grpdata_show_test_rderr;
			}
			retval = _cyttsp4_read_block_data(ts, ts->ic_grpoffset +
				ts->si_ofs.test_ofs, num_read, ic_buf,
				ts->platform_data->addr[CY_TCH_ADDR_OFS], true);
			if (retval < 0) {
				dev_err(ts->dev,
			"%s: Fail read Sysinfo ddata r=%d\n",
					__func__, retval);
				goto cyttsp4_ic_grpdata_show_test_rderr;
			}
			retval = _cyttsp4_set_mode(ts, CY_OPERATE_MODE);
			if (retval < 0) {
				dev_err(ts->dev,
			"%s: Fail enter Operational mode r=%d\n",
					__func__, retval);
			}
		}
		break;
cyttsp4_ic_grpdata_show_test_rderr:
		dev_err(ts->dev,
			"%s: Fail read test record\n", __func__);
		goto cyttsp4_ic_grpdata_show_prerr;
		break;
	case CY_IC_GRPNUM_PCFG_REC:
		num_read = ts->si_ofs.pcfg_size;
		dev_vdbg(ts->dev,
			"%s: GRP=PCFG_REC: num_read=%d at ofs=%d + grpofs=%d\n",
			__func__, num_read,
			ts->si_ofs.pcfg_ofs, ts->ic_grpoffset);
		if (ts->ic_grpoffset >= num_read)
			goto cyttsp4_ic_grpdata_show_ofserr;
		else {
			num_read -= ts->ic_grpoffset;
			retval = _cyttsp4_set_mode(ts, CY_SYSINFO_MODE);
			if (retval < 0) {
				dev_err(ts->dev,
			"%s: Fail enter Sysinfo mode r=%d\n",
					__func__, retval);
				goto cyttsp4_ic_grpdata_show_pcfg_rderr;
			}
			retval = _cyttsp4_read_block_data(ts, ts->ic_grpoffset +
				ts->si_ofs.pcfg_ofs, num_read, ic_buf,
				ts->platform_data->addr[CY_TCH_ADDR_OFS], true);
			if (retval < 0) {
				dev_err(ts->dev,
			"%s: Fail read Sysinfo ddata r=%d\n",
					__func__, retval);
				goto cyttsp4_ic_grpdata_show_pcfg_rderr;
			}
			retval = _cyttsp4_set_mode(ts, CY_OPERATE_MODE);
			if (retval < 0) {
				dev_err(ts->dev,
			"%s: Fail enter Operational mode r=%d\n",
					__func__, retval);
			}
		}
		break;
cyttsp4_ic_grpdata_show_pcfg_rderr:
		dev_err(ts->dev,
			"%s: Fail read pcfg record\n", __func__);
		goto cyttsp4_ic_grpdata_show_prerr;
		break;
	case CY_IC_GRPNUM_OPCFG_REC:
		num_read = ts->si_ofs.opcfg_size;
		dev_vdbg(ts->dev,
			"%s: GRP=OPCFG_REC:"
			" num_read=%d at ofs=%d + grpofs=%d\n",
			__func__, num_read,
			ts->si_ofs.opcfg_ofs, ts->ic_grpoffset);
		if (ts->ic_grpoffset >= num_read)
			goto cyttsp4_ic_grpdata_show_ofserr;
		else {
			num_read -= ts->ic_grpoffset;
			retval = _cyttsp4_set_mode(ts, CY_SYSINFO_MODE);
			if (retval < 0) {
				dev_err(ts->dev,
			"%s: Fail enter Sysinfo mode r=%d\n",
					__func__, retval);
				goto cyttsp4_ic_grpdata_show_opcfg_rderr;
			}
			retval = _cyttsp4_read_block_data(ts, ts->ic_grpoffset +
				ts->si_ofs.opcfg_ofs, num_read, ic_buf,
				ts->platform_data->addr[CY_TCH_ADDR_OFS], true);
			if (retval < 0) {
				dev_err(ts->dev,
			"%s: Fail read Sysinfo ddata r=%d\n",
					__func__, retval);
				goto cyttsp4_ic_grpdata_show_opcfg_rderr;
			}
			retval = _cyttsp4_set_mode(ts, CY_OPERATE_MODE);
			if (retval < 0) {
				dev_err(ts->dev,
			"%s: Fail enter Operational mode r=%d\n",
					__func__, retval);
			}
		}
		break;
cyttsp4_ic_grpdata_show_opcfg_rderr:
		dev_err(ts->dev,
			"%s: Fail read opcfg record\n", __func__);
		goto cyttsp4_ic_grpdata_show_prerr;
		break;
	case CY_IC_GRPNUM_TCH_PARM_VAL:

#ifdef CY_USE_TMA884
		ndata = CY_NUM_CONFIG_BYTES;
		/* do not show cmd, block size and end of block bytes */
		num_read = ndata - (6+4+6);
		dev_vdbg(ts->dev,
			"%s: GRP=PARM_VAL: num_read=%d at ofs=%d + grpofs=%d\n",
			__func__, num_read,
			0, ts->ic_grpoffset);
		if (ts->ic_grpoffset >= num_read)
			goto cyttsp4_ic_grpdata_show_ofserr;
		else {
			blockid = CY_TCH_PARM_EBID;
			pdata = kzalloc(ndata, GFP_KERNEL);
			if (pdata == NULL) {
				dev_err(ts->dev,
			"%s: Failed to allocate read buffer"
					" for %s\n",
					__func__, "platform_touch_param_data");
				retval = -ENOMEM;
				goto cyttsp4_ic_grpdata_show_tch_rderr;
			}
			dev_vdbg(ts->dev,
				"%s: read config block=0x%02X\n",
				__func__, blockid);
			retval = _cyttsp4_set_mode(ts, CY_CONFIG_MODE);
			if (retval < 0) {
				dev_err(ts->dev,
			"%s: Failed to switch to config mode\n",
					__func__);
				goto cyttsp4_ic_grpdata_show_tch_rderr;
			}
			retval = _cyttsp4_read_config_block(ts,
				blockid, pdata, ndata,
				"platform_touch_param_data");
			if (retval < 0) {
				dev_err(ts->dev,
			"%s: Failed read config block %s r=%d\n",
					__func__, "platform_touch_param_data",
					retval);
				goto cyttsp4_ic_grpdata_show_tch_rderr;
			}
			retval = _cyttsp4_set_mode(ts, CY_OPERATE_MODE);
			if (retval < 0) {
				_cyttsp4_change_state(ts, CY_IDLE_STATE);
				dev_err(ts->dev,
			"%s: Fail set operational mode (r=%d)\n",
					__func__, retval);
				goto cyttsp4_ic_grpdata_show_tch_rderr;
			}
			dev_vdbg(ts->dev,
				"%s: memcpy config block=0x%02X\n",
				__func__, blockid);
			num_read -= ts->ic_grpoffset;
			/*
			 * cmd+rdy_bit, status, ebid, lenh, lenl, reserved,
			 * data[0] .. data[ndata-6]
			 * skip data[0] .. data[3] - block size bytes
			 */
			memcpy(ic_buf,
				&pdata[6+4] + ts->ic_grpoffset, num_read);
			kfree(pdata);
		}
		break;
#endif /* --CY_USE_TMA884 */
cyttsp4_ic_grpdata_show_tch_rderr:
		if (pdata != NULL)
			kfree(pdata);
		goto cyttsp4_ic_grpdata_show_prerr;
	case CY_IC_GRPNUM_TCH_PARM_SIZ:
		if (ts->platform_data->sett
			[CY_IC_GRPNUM_TCH_PARM_SIZ] == NULL) {
			dev_err(ts->dev,
			"%s: Missing platform data"
				" Touch Parameters Sizes table\n", __func__);
			goto cyttsp4_ic_grpdata_show_prerr;
		}
		if (ts->platform_data->sett
			[CY_IC_GRPNUM_TCH_PARM_SIZ]->data == NULL) {
			dev_err(ts->dev,
			"%s: Missing platform data"
				" Touch Parameters Sizes table data\n",
				__func__);
			goto cyttsp4_ic_grpdata_show_prerr;
		}
		num_read = ts->platform_data->sett
			[CY_IC_GRPNUM_TCH_PARM_SIZ]->size;
		dev_vdbg(ts->dev,
			"%s: GRP=PARM_SIZ:"
			" num_read=%d at ofs=%d + grpofs=%d\n",
			__func__, num_read,
			0, ts->ic_grpoffset);
		if (ts->ic_grpoffset >= num_read)
			goto cyttsp4_ic_grpdata_show_ofserr;
		else {
			num_read -= ts->ic_grpoffset;
			memcpy(ic_buf, (u8 *)ts->platform_data->sett
				[CY_IC_GRPNUM_TCH_PARM_SIZ]->data +
				ts->ic_grpoffset, num_read);
		}
		break;
	case CY_IC_GRPNUM_DDATA_REC:
		num_read = ts->si_ofs.ddata_size;
		dev_vdbg(ts->dev,
			"%s: GRP=DDATA_REC:"
			" num_read=%d at ofs=%d + grpofs=%d\n",
			__func__, num_read,
			ts->si_ofs.ddata_ofs, ts->ic_grpoffset);
		if (ts->ic_grpoffset >= num_read)
			goto cyttsp4_ic_grpdata_show_ofserr;
		else {
			num_read -= ts->ic_grpoffset;
			retval = _cyttsp4_set_mode(ts, CY_SYSINFO_MODE);
			if (retval < 0) {
				dev_err(ts->dev,
			"%s: Fail enter Sysinfo mode r=%d\n",
					__func__, retval);
				goto cyttsp4_ic_grpdata_show_ddata_rderr;
			}
			retval = _cyttsp4_read_block_data(ts, ts->ic_grpoffset +
				ts->si_ofs.ddata_ofs, num_read, ic_buf,
				ts->platform_data->addr[CY_TCH_ADDR_OFS], true);
			if (retval < 0) {
				dev_err(ts->dev,
			"%s: Fail read Sysinfo ddata r=%d\n",
					__func__, retval);
				goto cyttsp4_ic_grpdata_show_ddata_rderr;
			}
			retval = _cyttsp4_set_mode(ts, CY_OPERATE_MODE);
			if (retval < 0) {
				dev_err(ts->dev,
			"%s: Fail enter Operational mode r=%d\n",
					__func__, retval);
			}
		}
		break;
cyttsp4_ic_grpdata_show_ddata_rderr:
		dev_err(ts->dev,
			"%s: Fail read ddata\n", __func__);
		goto cyttsp4_ic_grpdata_show_prerr;
		break;
	case CY_IC_GRPNUM_MDATA_REC:
		num_read = ts->si_ofs.mdata_size;
		dev_vdbg(ts->dev,
			"%s: GRP=MDATA_REC:"
			" num_read=%d at ofs=%d + grpofs=%d\n",
			__func__, num_read,
			ts->si_ofs.mdata_ofs, ts->ic_grpoffset);
		if (ts->ic_grpoffset >= num_read)
			goto cyttsp4_ic_grpdata_show_ofserr;
		else {
			num_read -= ts->ic_grpoffset;
			retval = _cyttsp4_set_mode(ts, CY_SYSINFO_MODE);
			if (retval < 0) {
				dev_err(ts->dev,
			"%s: Fail enter Sysinfo mode r=%d\n",
					__func__, retval);
				goto cyttsp4_ic_grpdata_show_mdata_rderr;
			}
			retval = _cyttsp4_read_block_data(ts, ts->ic_grpoffset +
				ts->si_ofs.mdata_ofs, num_read, ic_buf,
				ts->platform_data->addr[CY_TCH_ADDR_OFS], true);
			if (retval < 0) {
				dev_err(ts->dev,
			"%s: Fail read Sysinfo regs r=%d\n",
					__func__, retval);
				goto cyttsp4_ic_grpdata_show_mdata_rderr;
			}
			retval = _cyttsp4_set_mode(ts, CY_OPERATE_MODE);
			if (retval < 0) {
				dev_err(ts->dev,
			"%s: Fail enter Operational mode r=%d\n",
					__func__, retval);
			}
		}
		break;
cyttsp4_ic_grpdata_show_mdata_rderr:
		dev_err(ts->dev,
			"%s: Fail read mdata\n", __func__);
		goto cyttsp4_ic_grpdata_show_prerr;
		break;
	case CY_IC_GRPNUM_TEST_REGS:
		if (ts->test.cur_cmd == CY_TEST_CMD_NULL) {
			num_read = 1;
			retval = _cyttsp4_load_status_regs(ts);
			if (retval < 0) {
				dev_err(ts->dev,
			"%s: failed to read host mode"
					" r=%d\n", __func__, retval);
				ic_buf[0] = (u8)CY_IGNORE_VALUE;
			} else
				ic_buf[0] = ts->xy_mode[0];
			dev_vdbg(ts->dev,
				"%s: GRP=TEST_REGS: NULL CMD: host_mode"
				"=%02X\n", __func__, ic_buf[0]);
		} else if (ts->test.cur_mode == CY_TEST_MODE_CAT) {
			num_read = ts->test.cur_status_size;
			dev_vdbg(ts->dev,
				"%s: GRP=TEST_REGS: num_rd=%d at ofs=%d"
				" + grpofs=%d\n", __func__, num_read,
				ts->si_ofs.cmd_ofs, ts->ic_grpoffset);
			retval = _cyttsp4_read_block_data(ts,
				ts->ic_grpoffset + ts->si_ofs.cmd_ofs,
				num_read, ic_buf,
				ts->platform_data->addr
				[CY_TCH_ADDR_OFS], true);
			if (retval < 0)
				goto cyttsp4_ic_grpdata_show_prerr;
		} else
			dev_err(ts->dev,
			"%s: Not in Config/Test mode\n", __func__);
		break;
	default:
		goto cyttsp4_ic_grpdata_show_grperr;
		break;
	}

	snprintf(buf, CY_MAX_PRBUF_SIZE,
		"Group %d, Offset %u:\n", ts->ic_grpnum, ts->ic_grpoffset);
	for (i = 0; i < num_read; i++) {
		snprintf(buf, CY_MAX_PRBUF_SIZE,
			"%s0x%02X\n", buf, ic_buf[i]);
	}
	kfree(ic_buf);
	return snprintf(buf, CY_MAX_PRBUF_SIZE,
		"%s(%d bytes)\n", buf, num_read);

cyttsp4_ic_grpdata_show_ofserr:
	dev_err(ts->dev,
			"%s: Group Offset=%d exceeds Group Read Length=%d\n",
		__func__, ts->ic_grpoffset, num_read);
	kfree(ic_buf);
	snprintf(buf, CY_MAX_PRBUF_SIZE,
		"Cannot read Group %d Data.\n",
		ts->ic_grpnum);
	return snprintf(buf, CY_MAX_PRBUF_SIZE,
		"%sGroup Offset=%d exceeds Group Read Length=%d\n",
		buf, ts->ic_grpoffset, num_read);
cyttsp4_ic_grpdata_show_prerr:
	dev_err(ts->dev,
			"%s: Cannot read Group %d Data.\n",
		__func__, ts->ic_grpnum);
	kfree(ic_buf);
	return snprintf(buf, CY_MAX_PRBUF_SIZE,
		"Cannot read Group %d Data.\n",
		ts->ic_grpnum);
cyttsp4_ic_grpdata_show_grperr:
	dev_err(ts->dev,
			"%s: Group %d does not exist.\n",
		__func__, ts->ic_grpnum);
	kfree(ic_buf);
	return snprintf(buf, CY_MAX_PRBUF_SIZE,
		"Group %d does not exist.\n",
		ts->ic_grpnum);
}
static ssize_t cyttsp4_ic_grpdata_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct cyttsp4 *ts = dev_get_drvdata(dev);
	ssize_t retval = 0;

	mutex_lock(&ts->data_lock);
	if (ts->driver_state == CY_SLEEP_STATE) {
		dev_err(ts->dev,
			"%s: Group Show Test blocked: IC suspended\n",
			__func__);
		retval = snprintf(buf, CY_MAX_PRBUF_SIZE,
			"Group %d Show Test blocked: IC suspended\n",
			ts->ic_grpnum);
	} else
		retval = _cyttsp4_ic_grpdata_show(dev, attr, buf);
	mutex_unlock(&ts->data_lock);

	return retval;
}


#ifdef CY_USE_TMA884
static int _cyttsp4_write_mddata(struct cyttsp4 *ts, size_t write_length,
	size_t mddata_length, u8 blkid, size_t mddata_ofs,
	u8 *ic_buf, const char *mddata_name)
{
	bool mddata_updated = false;
	u8 *pdata;
	int retval = 0;

	pdata = kzalloc(CY_MAX_PRBUF_SIZE, GFP_KERNEL);
	if (pdata == NULL) {
		dev_err(ts->dev,
			"%s: Fail allocate data buffer\n", __func__);
		retval = -ENOMEM;
		goto cyttsp4_write_mddata_exit;
	}
	if (ts->current_mode != CY_MODE_OPERATIONAL) {
		dev_err(ts->dev,
			"%s: Must be in operational mode to start write of"
			" %s (current mode=%d)\n",
			__func__, mddata_name, ts->current_mode);
		retval = -EPERM;
		goto cyttsp4_write_mddata_exit;
	}
	if ((write_length + ts->ic_grpoffset) > mddata_length) {
		dev_err(ts->dev,
			"%s: Requested length(%d) is greater than"
			" %s size(%d)\n", __func__,
			write_length, mddata_name, mddata_length);
		retval = -EINVAL;
		goto cyttsp4_write_mddata_exit;
	}
	retval = _cyttsp4_set_mode(ts, CY_SYSINFO_MODE);
	if (retval < 0) {
		dev_err(ts->dev,
			"%s: Fail to enter Sysinfo mode r=%d\n",
			__func__, retval);
		goto cyttsp4_write_mddata_exit;
	}
	dev_vdbg(ts->dev,
		"%s: blkid=%02X mddata_ofs=%d mddata_length=%d"
		" mddata_name=%s write_length=%d grpofs=%d\n",
		__func__, blkid, mddata_ofs, mddata_length, mddata_name,
		write_length, ts->ic_grpoffset);
	_cyttsp4_read_block_data(ts, mddata_ofs, mddata_length, pdata,
		ts->platform_data->addr[CY_TCH_ADDR_OFS], true);
	if (retval < 0) {
		dev_err(ts->dev,
			"%s: Fail to read %s regs r=%d\n",
			__func__, mddata_name, retval);
		goto cyttsp4_write_mddata_exit;
	}
	memcpy(pdata + ts->ic_grpoffset, ic_buf, write_length);
	_cyttsp4_set_data_block(ts, blkid, pdata,
		mddata_length, mddata_name, true, &mddata_updated);
	if ((retval < 0) || !mddata_updated) {
		dev_err(ts->dev,
			"%s: Fail while writing %s block r=%d updated=%d\n",
			__func__, mddata_name, retval, (int)mddata_updated);
	}
	retval = _cyttsp4_set_mode(ts, CY_OPERATE_MODE);
	if (retval < 0) {
		dev_err(ts->dev,
			"%s: Fail to enter Operational mode r=%d\n",
			__func__, retval);
	}

cyttsp4_write_mddata_exit:
	kfree(pdata);
	return retval;
}
#endif /* --CY_USE_TMA884 */

static ssize_t _cyttsp4_ic_grpdata_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	struct cyttsp4 *ts = dev_get_drvdata(dev);
	unsigned long value = 0;
	int retval = 0;
	const char *pbuf = buf;
	int i = 0;
	int j = 0;
	char last = 0;
	char *scan_buf = NULL;
	u8 *ic_buf = NULL;
	size_t length = 0;
	u8 host_mode = 0;
	enum cyttsp4_driver_state save_state = CY_INVALID_STATE;
#ifdef CY_USE_TMA884
	u8 *pdata = NULL;
	size_t mddata_length = 0;
	size_t ndata = 0;
	u8 blockid = 0;
	bool mddata_updated = false;
	const char *mddata_name = "invalid name";
#endif /* --CY_USE_TMA884 */

	scan_buf = kzalloc(CY_MAX_PRBUF_SIZE, GFP_KERNEL);
	if (scan_buf == NULL) {
		dev_err(ts->dev,
			"%s: Failed to allocate scan buffer for"
			" Group Data store\n", __func__);
		goto cyttsp4_ic_grpdata_store_exit;
	}
	ic_buf = kzalloc(CY_MAX_PRBUF_SIZE, GFP_KERNEL);
	if (ic_buf == NULL) {
		dev_err(ts->dev,
			"%s: Failed to allocate ic buffer for"
			" Group Data store\n", __func__);
		goto cyttsp4_ic_grpdata_store_exit;
	}
	dev_vdbg(ts->dev,
		"%s: grpnum=%d grpoffset=%u\n",
		__func__, ts->ic_grpnum, ts->ic_grpoffset);

	if (ts->ic_grpnum >= CY_IC_GRPNUM_NUM) {
		dev_err(ts->dev,
			"%s: Group %d does not exist.\n",
			__func__, ts->ic_grpnum);
		retval = size;
		goto cyttsp4_ic_grpdata_store_exit;
	}
	dev_vdbg(ts->dev,
		"%s: pbuf=%p buf=%p size=%d sizeof(scan_buf)=%d buf=%s\n",
		__func__, pbuf, buf, size, sizeof(scan_buf), buf);

	i = 0;
	last = 0;
	while (pbuf <= (buf + size)) {
		while (((*pbuf == ' ') || (*pbuf == ',')) &&
			(pbuf < (buf + size))) {
			last = *pbuf;
			pbuf++;
		}
		if (pbuf < (buf + size)) {
			memset(scan_buf, 0, CY_MAX_PRBUF_SIZE);
			if ((last == ',') && (*pbuf == ',')) {
				dev_err(ts->dev,
			"%s: Invalid data format. "
					"\",,\" not allowed.\n",
					__func__);
				retval = size;
				goto cyttsp4_ic_grpdata_store_exit;
			}
			for (j = 0; j < sizeof("0xHH") &&
				*pbuf != ' ' && *pbuf != ','; j++) {
				last = *pbuf;
				scan_buf[j] = *pbuf++;
			}
			retval = strict_strtoul(scan_buf, 16, &value);
			if (retval < 0) {
				dev_err(ts->dev,
			"%s: Invalid data format. "
					"Use \"0xHH,...,0xHH\" instead.\n",
					__func__);
				retval = size;
				goto cyttsp4_ic_grpdata_store_exit;
			} else {
				if (i >= ts->max_config_bytes) {
					dev_err(ts->dev,
			"%s: Max command size exceeded"
					" (size=%d max=%d)\n", __func__,
					i, ts->max_config_bytes);
					goto cyttsp4_ic_grpdata_store_exit;
				}
				ic_buf[i] = value;
				dev_vdbg(ts->dev,
					"%s: ic_buf[%d] = 0x%02X\n",
					__func__, i, ic_buf[i]);
				i++;
			}
		} else
			break;
	}
	length = i;

	/* write ic_buf to log */
	_cyttsp4_pr_buf(ts, ic_buf, length, "ic_buf");

	switch (ts->ic_grpnum) {
	case CY_IC_GRPNUM_CMD_REGS:
		if ((length + ts->ic_grpoffset + ts->si_ofs.cmd_ofs) >
			ts->si_ofs.rep_ofs) {
			dev_err(ts->dev,
			"%s: Length(%d) + offset(%d) + cmd_offset(%d)"
				" is beyond cmd reg space[%d..%d]\n", __func__,
				length, ts->ic_grpoffset, ts->si_ofs.cmd_ofs,
				ts->si_ofs.cmd_ofs, ts->si_ofs.rep_ofs - 1);
			goto cyttsp4_ic_grpdata_store_exit;
		}
		retval = _cyttsp4_write_block_data(ts, ts->ic_grpoffset +
			ts->si_ofs.cmd_ofs, length, ic_buf,
			ts->platform_data->addr[CY_TCH_ADDR_OFS], true);
		if (retval < 0) {
			dev_err(ts->dev,
			"%s: Fail write command regs r=%d\n",
				__func__, retval);
		}
		if (!ts->ic_grptest) {
			dev_info(ts->dev,
			"%s: Disabled settings checksum verifications"
				" until next boot.\n", __func__);
			ts->ic_grptest = true;
		}
		break;
	case CY_IC_GRPNUM_TCH_PARM_VAL:
#ifdef CY_USE_TMA884
		mddata_name = "Touch Parameters";
		ndata = CY_NUM_CONFIG_BYTES;
		blockid = CY_TCH_PARM_EBID;
		/* do not show cmd, block size and end of block bytes */
		mddata_length = ndata - (6+4+6);
		dev_vdbg(ts->dev,
			"%s: GRP=PARM_VAL: write length=%d at ofs=%d +"
			" grpofs=%d\n", __func__, length,
			0, ts->ic_grpoffset);
		if ((length + ts->ic_grpoffset) > mddata_length) {
			dev_err(ts->dev,
			"%s: Requested length(%d) is greater than"
				" %s size(%d)\n", __func__,
				length, mddata_name, mddata_length);
			retval = -EINVAL;
			goto cyttsp4_ic_grpdata_store_tch_wrerr;
		}
		pdata = kzalloc(ndata, GFP_KERNEL);
		if (pdata == NULL) {
			dev_err(ts->dev,
			"%s: Failed to al_cyttsp4_ic_grpdata_storelocate read/write buffer"
				" for %s\n",
				__func__, "platform_touch_param_data");
			retval = -ENOMEM;
			goto cyttsp4_ic_grpdata_store_tch_wrerr;
		}
		dev_vdbg(ts->dev,
			"%s: read config block=0x%02X\n",
			__func__, blockid);
		retval = _cyttsp4_set_mode(ts, CY_CONFIG_MODE);
		if (retval < 0) {
			dev_err(ts->dev,
			"%s: Failed to switch to config mode\n",
				__func__);
			goto cyttsp4_ic_grpdata_store_tch_wrerr;
		}
		retval = _cyttsp4_read_config_block(ts,
			blockid, pdata, ndata,
			"platform_touch_param_data");
		if (retval < 0) {
			dev_err(ts->dev,
			"%s: Failed read config block %s r=%d\n",
				__func__, "platform_touch_param_data",
				retval);
			goto cyttsp4_ic_grpdata_store_tch_wrerr;
		}
		/*
		 * cmd+rdy_bit, status, ebid, lenh, lenl, reserved,
		 * data[0] .. data[ndata-6]
		 * skip data[0] .. data[3] - block size bytes
		 */
		memcpy(&pdata[6+4+ts->ic_grpoffset], ic_buf, length);
		_cyttsp4_set_data_block(ts, blockid, &pdata[6+4],
			mddata_length, mddata_name, true, &mddata_updated);
		if ((retval < 0) || !mddata_updated) {
			dev_err(ts->dev,
			"%s: Fail while writing %s block r=%d"
				" updated=%d\n", __func__,
				mddata_name, retval, (int)mddata_updated);
		}
		if (!ts->ic_grptest) {
			dev_info(ts->dev,
			"%s: Disabled settings checksum verifications"
				" until next boot.\n", __func__);
			ts->ic_grptest = true;
		}
		retval = _cyttsp4_startup(ts);

		if (retval < 0) {
			dev_err(ts->dev,
			"%s: Fail restart after writing params r=%d\n",
				__func__, retval);
		}
cyttsp4_ic_grpdata_store_tch_wrerr:
		kfree(pdata);
		break;
#endif /* --CY_USE_TMA884 */
	case CY_IC_GRPNUM_DDATA_REC:
#ifdef CY_USE_TMA884
		mddata_length = ts->si_ofs.ddata_size;
		dev_vdbg(ts->dev,
			"%s: DDATA_REC length=%d mddata_length=%d blkid=%02X"
			" ddata_ofs=%d name=%s\n", __func__, length,
			mddata_length, CY_DDATA_EBID, ts->si_ofs.ddata_ofs,
			"Design Data");
		_cyttsp4_pr_buf(ts, ic_buf, length, "Design Data");
		retval = _cyttsp4_write_mddata(ts, length, mddata_length,
			CY_DDATA_EBID, ts->si_ofs.ddata_ofs, ic_buf,
			"Design Data");
		if (retval < 0) {
			dev_err(ts->dev,
			"%s: Fail writing Design Data\n",
				__func__);
		} else if (!ts->ic_grptest) {
			dev_info(ts->dev,
			"%s: Disabled settings checksum verifications"
				" until next boot.\n", __func__);
			ts->ic_grptest = true;
		}
		break;
#endif /* --CY_USE_TMA884 */
	case CY_IC_GRPNUM_MDATA_REC:

#ifdef CY_USE_TMA884
		mddata_length = ts->si_ofs.mdata_size;
		dev_vdbg(ts->dev,
			"%s: MDATA_REC length=%d mddata_length=%d blkid=%02X"
			" ddata_ofs=%d name=%s\n", __func__, length,
			mddata_length, CY_MDATA_EBID, ts->si_ofs.mdata_ofs,
			"Manufacturing Data");
		_cyttsp4_pr_buf(ts, ic_buf, length, "Manufacturing Data");
		retval = _cyttsp4_write_mddata(ts, length, mddata_length,
			CY_MDATA_EBID, ts->si_ofs.mdata_ofs, ic_buf,
			"Manufacturing Data");
		if (retval < 0) {
			dev_err(ts->dev,
			"%s: Fail writing Manufacturing Data\n",
				__func__);
		} else if (!ts->ic_grptest) {
			dev_info(ts->dev,
			"%s: Disabled settings checksum verifications"
				" until next boot.\n", __func__);
			ts->ic_grptest = true;
		}
		break;
#endif /* --CY_USE_TMA884 */
	case CY_IC_GRPNUM_TEST_REGS:
		ts->test.cur_cmd = ic_buf[0];
		if (ts->test.cur_cmd == CY_TEST_CMD_NULL) {
			switch (ic_buf[1]) {
			case CY_NULL_CMD_NULL:
				dev_err(ts->dev,
			"%s: empty NULL command\n", __func__);
				break;
			case CY_NULL_CMD_MODE:
				save_state = ts->driver_state;
				_cyttsp4_change_state(ts, CY_CMD_STATE);
				host_mode = ic_buf[2] | CY_MODE_CHANGE;
				retval = _cyttsp4_write_block_data(ts,
					CY_REG_BASE, sizeof(host_mode),
					&host_mode, ts->platform_data->addr
					[CY_TCH_ADDR_OFS], true);
				if (retval < 0) {
					dev_err(ts->dev,
			"%s: Fail write host_mode=%02X"
					" r=%d\n", __func__, ic_buf[2], retval);
				} else {
					INIT_COMPLETION(ts->int_running);
					retval = _cyttsp4_wait_int_no_init(ts,
						CY_HALF_SEC_TMO_MS * 5);
					if (retval < 0) {
						dev_err(ts->dev,
			"%s: timeout waiting"
						" host_mode=0x%02X"
						" change  r=%d\n",
						__func__, ic_buf[1], retval);
						/* continue anyway */
					}
					retval = _cyttsp4_cmd_handshake(ts);
					if (retval < 0) {
						dev_err(ts->dev,
			"%s: Fail mode handshake"
							" r=%d\n",
							__func__, retval);
					}
					if (GET_HSTMODE(ic_buf[2]) ==
						GET_HSTMODE(CY_CONFIG_MODE)) {
						ts->test.cur_mode =
							CY_TEST_MODE_CAT;
					} else {
						ts->test.cur_mode =
							CY_TEST_MODE_NORMAL_OP;
					}
				}
				_cyttsp4_change_state(ts, save_state);
				break;
			case CY_NULL_CMD_STATUS_SIZE:
				ts->test.cur_status_size = ic_buf[2] +
					(ic_buf[3] * 256);
				break;
			case CY_NULL_CMD_HANDSHAKE:
				retval = _cyttsp4_cmd_handshake(ts);
				if (retval < 0) {
					dev_err(ts->dev,
			"%s: Fail test cmd handshake"
						" r=%d\n",
						__func__, retval);
				}
			default:
				break;
			}
		} else {
			dev_dbg(ts->dev,
				"%s: TEST CMD=0x%02X length=%d"
				" cmd_ofs+grpofs=%d\n", __func__, ic_buf[0],
				length, ts->ic_grpoffset + ts->si_ofs.cmd_ofs);
			_cyttsp4_pr_buf(ts, ic_buf, length, "test_cmd");
			retval = _cyttsp4_write_block_data(ts,
				ts->ic_grpoffset + ts->si_ofs.cmd_ofs,
				length, ic_buf,
				ts->platform_data->addr[CY_TCH_ADDR_OFS], true);
			if (retval < 0) {
				dev_err(ts->dev,
			"%s: Fail write command regs r=%d\n",
					__func__, retval);
			}
		}
		break;
	default:
		dev_err(ts->dev,
			"%s: Group=%d is read only\n",
			__func__, ts->ic_grpnum);
		break;
	}

cyttsp4_ic_grpdata_store_exit:
	if (scan_buf != NULL)
		kfree(scan_buf);
	if (ic_buf != NULL)
		kfree(ic_buf);
	return size;
}
static ssize_t cyttsp4_ic_grpdata_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	struct cyttsp4 *ts = dev_get_drvdata(dev);
	ssize_t retval = 0;

	mutex_lock(&ts->data_lock);
	if (ts->driver_state == CY_SLEEP_STATE) {
		dev_err(ts->dev,
			"%s: Group Store Test blocked: IC suspended\n",
			__func__);
		retval = size;
	} else
		retval = _cyttsp4_ic_grpdata_store(dev, attr, buf, size);
	mutex_unlock(&ts->data_lock);

	return retval;
}
static DEVICE_ATTR(ic_grpdata, S_IRWXU | S_IRWXG | S_IRWXO,
	cyttsp4_ic_grpdata_show, cyttsp4_ic_grpdata_store);

static ssize_t cyttsp4_drv_flags_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct cyttsp4 *ts = dev_get_drvdata(dev);

	return snprintf(buf, CY_MAX_PRBUF_SIZE,
		"Current Driver Flags: 0x%04X\n", ts->flags);
}
static ssize_t cyttsp4_drv_flags_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	struct cyttsp4 *ts = dev_get_drvdata(dev);
	unsigned long value = 0;
	ssize_t retval = 0;

	mutex_lock(&(ts->data_lock));
	retval = strict_strtoul(buf, 16, &value);
	if (retval < 0) {
		dev_err(ts->dev,
			"%s: Failed to convert value\n", __func__);
		goto cyttsp4_drv_flags_store_error_exit;
	}

	if (value > 0xFFFF) {
		dev_err(ts->dev,
			"%s: value=%lu is greater than max;"
			" drv_flags=0x%04X\n", __func__, value, ts->flags);
	} else {
		ts->flags = value;
	}

	dev_vdbg(ts->dev,
		"%s: drv_flags=0x%04X\n", __func__, ts->flags);

cyttsp4_drv_flags_store_error_exit:
	retval = size;
	mutex_unlock(&(ts->data_lock));
	return retval;
}
static DEVICE_ATTR(drv_flags, S_IRUSR | S_IWUSR,
	cyttsp4_drv_flags_show, cyttsp4_drv_flags_store);

static ssize_t cyttsp4_hw_reset_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	struct cyttsp4 *ts = dev_get_drvdata(dev);
	ssize_t retval = 0;

	mutex_lock(&(ts->data_lock));
	retval = _cyttsp4_startup(ts);
	mutex_unlock(&(ts->data_lock));
	if (retval < 0) {
		dev_err(ts->dev,
			"%s: fail hw_reset device restart r=%d\n",
			__func__, retval);
	}

	retval = size;
	return retval;
}
static DEVICE_ATTR(hw_reset, S_IWUSR, NULL, cyttsp4_hw_reset_store);

static ssize_t cyttsp4_hw_recov_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	struct cyttsp4 *ts = dev_get_drvdata(dev);
	unsigned long value = 0;
	ssize_t retval = 0;

	mutex_lock(&(ts->data_lock));
	retval = strict_strtoul(buf, 10, &value);
	if (retval < 0) {
		dev_err(ts->dev,
			"%s: Failed to convert value\n", __func__);
		goto cyttsp4_hw_recov_store_error_exit;
	}

	if (ts->platform_data->hw_recov == NULL) {
		dev_err(ts->dev,
			"%s: no hw_recov function\n", __func__);
		goto cyttsp4_hw_recov_store_error_exit;
	}

	retval = ts->platform_data->hw_recov((int)value);
	if (retval < 0) {
		dev_err(ts->dev,
			"%s: fail hw_recov(value=%d) function r=%d\n",
			__func__, (int)value, retval);
	}

cyttsp4_hw_recov_store_error_exit:
	retval = size;
	mutex_unlock(&(ts->data_lock));
	return retval;
}
static DEVICE_ATTR(hw_recov, S_IWUSR, NULL, cyttsp4_hw_recov_store);
#endif /* --CONFIG_TOUCHSCREEN_DEBUG */

#ifdef CONFIG_TOUCHSCREEN_DEBUG_ENABLE_ENTRY
static ssize_t cyttsp4_ts_debug_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct cyttsp4 *ts = dev_get_drvdata(dev);

	return snprintf(buf, CY_MAX_PRBUF_SIZE,
		"%s\n",
		(ts->debug_enable == true) ? "enable":"disable");
}

static ssize_t cyttsp4_ts_debug_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	struct cyttsp4 *ts = dev_get_drvdata(dev);
	char messages[256] = {'\0'};

	strncpy(messages, buf, size);

	if(strncmp(messages, "enable", size-1) == 0) {
		ts->debug_enable = true;
	}
	if(strncmp(messages, "disable", size-1) == 0) {
		ts->debug_enable = false;
	}

	return size;
}
static DEVICE_ATTR(ts_debug, S_IRUSR | S_IWUSR,
	cyttsp4_ts_debug_show, cyttsp4_ts_debug_store);
#endif

#define CY_CMD_I2C_ADDR					0
#define CY_STATUS_SIZE_BYTE				1
#define CY_STATUS_TYP_DELAY				2
#define CY_CMD_TAIL_LEN					3
#define CY_CMD_BYTE					1
#define CY_STATUS_BYTE					1
#define CY_MAX_STATUS_SIZE				32
#define CY_MIN_STATUS_SIZE				5
#define CY_START_OF_PACKET				0x01
#define CY_END_OF_PACKET				0x17
#define CY_DATA_ROW_SIZE				288
#define CY_DATA_ROW_SIZE_TMA400				128
#define CY_PACKET_DATA_LEN				96
#define CY_MAX_PACKET_LEN				512
#define CY_COMM_BUSY					0xFF
#define CY_CMD_BUSY					0xFE
#define CY_SEPARATOR_OFFSET				0
#define CY_ARRAY_ID_OFFSET				0
#define CY_ROW_NUM_OFFSET				1
#define CY_ROW_SIZE_OFFSET				3
#define CY_ROW_DATA_OFFSET				5
#define CY_FILE_SILICON_ID_OFFSET			0
#define CY_FILE_REV_ID_OFFSET				4
#define CY_CMD_LDR_HOST_SYNC				0xFF /* tma400 */
#define CY_CMD_LDR_EXIT					0x3B
#define CY_CMD_LDR_EXIT_CMD_SIZE			7
#define CY_CMD_LDR_EXIT_STAT_SIZE			7

enum ldr_status {
	ERROR_SUCCESS = 0,
	ERROR_COMMAND = 1,
	ERROR_FLASH_ARRAY = 2,
	ERROR_PACKET_DATA = 3,
	ERROR_PACKET_LEN = 4,
	ERROR_PACKET_CHECKSUM = 5,
	ERROR_FLASH_PROTECTION = 6,
	ERROR_FLASH_CHECKSUM = 7,
	ERROR_VERIFY_IMAGE = 8,
	ERROR_UKNOWN1 = 9,
	ERROR_UKNOWN2 = 10,
	ERROR_UKNOWN3 = 11,
	ERROR_UKNOWN4 = 12,
	ERROR_UKNOWN5 = 13,
	ERROR_UKNOWN6 = 14,
	ERROR_INVALID_COMMAND = 15,
	ERROR_INVALID
};

static u16 _cyttsp4_compute_crc(struct cyttsp4 *ts, u8 *buf, int size)
{
	u16 crc = 0xffff;
	u16 tmp;
	int i;

	/* RUN CRC */

	if (size == 0)
		crc = ~crc;
	else {

		do {
			for (i = 0, tmp = 0x00ff & *buf++; i < 8;
				i++, tmp >>= 1) {
				if ((crc & 0x0001) ^ (tmp & 0x0001))
					crc = (crc >> 1) ^ 0x8408;
				else
					crc >>= 1;
			}
		} while (--size);

		crc = ~crc;
		tmp = crc;
		crc = (crc << 8) | (tmp >> 8 & 0xFF);
	}

	return crc;
}

static int _cyttsp4_get_status(struct cyttsp4 *ts,
	u8 *buf, int size, unsigned long timeout_ms)
{
	unsigned long uretval = 0;
	int tries = 0;
	int retval = 0;

	if (timeout_ms != 0) {
		/* wait until status ready interrupt or timeout occurs */
		uretval = wait_for_completion_interruptible_timeout(
			&ts->int_running, msecs_to_jiffies(timeout_ms));

		/* read the status packet */
		if (buf == NULL) {
			dev_err(ts->dev,
			"%s: Status buf ptr is NULL\n", __func__);
			retval = -EINVAL;
			goto _cyttsp4_get_status_exit;
		}
		for (tries = 0; tries < 2; tries++) {
			retval = _cyttsp4_read_block_data(ts, CY_REG_BASE, size,
				buf, ts->platform_data->addr[CY_LDR_ADDR_OFS],
				false);
			/*
			 * retry if bus read error or
			 * status byte shows not ready
			 */
			if ((buf[1] == CY_COMM_BUSY) || (buf[1] == CY_CMD_BUSY))
				msleep(CY_DELAY_DFLT);
			else
				break;
		}
		dev_vdbg(ts->dev,
			"%s: tries=%d ret=%d status=%02X\n",
			__func__, tries, retval, buf[1]);
	}

_cyttsp4_get_status_exit:
	mutex_lock(&ts->data_lock);
	return retval;
}

/*
 * Send a bootloader command to the device;
 * Wait for the ISR to execute indicating command
 * was received and status is ready;
 * Releases data_lock mutex to allow ISR to run,
 * then locks it again.
 */
static int _cyttsp4_send_cmd(struct cyttsp4 *ts, const u8 *cmd_buf,
	int cmd_size, u8 *stat_ret, size_t num_stat_byte,
	size_t status_size, unsigned long timeout_ms)
{
	u8 *status_buf = NULL;
	int retval = 0;

	if (timeout_ms > 0) {
		status_buf = kzalloc(CY_MAX_STATUS_SIZE, GFP_KERNEL);
		if (status_buf == NULL) {
			dev_err(ts->dev,
			"%s: Fail alloc status buffer=%p\n",
				__func__, status_buf);
			goto _cyttsp4_send_cmd_exit;
		}
	}

	if (cmd_buf == NULL) {
		dev_err(ts->dev,
			"%s: bad cmd_buf=%p\n", __func__, cmd_buf);
		goto _cyttsp4_send_cmd_exit;
	}

	if (cmd_size == 0) {
		dev_err(ts->dev,
			"%s: bad cmd_size=%d\n", __func__, cmd_size);
		goto _cyttsp4_send_cmd_exit;
	}

	_cyttsp4_pr_buf(ts, (u8 *)cmd_buf, cmd_size, "send_cmd");

	mutex_unlock(&ts->data_lock);
	if (timeout_ms > 0)
		INIT_COMPLETION(ts->int_running);
	retval = _cyttsp4_write_block_data(ts, CY_REG_BASE, cmd_size, cmd_buf,
		ts->platform_data->addr[CY_LDR_ADDR_OFS],
		false);

	if (retval < 0) {
		dev_err(ts->dev,
			"%s: Fail writing command=%02X\n",
			__func__, cmd_buf[CY_CMD_BYTE]);
		mutex_lock(&ts->data_lock);
		goto _cyttsp4_send_cmd_exit;
	}

	/* get the status and lock the mutex */
	if (timeout_ms > 0) {
		retval = _cyttsp4_get_status(ts, status_buf,
			status_size, timeout_ms);
		if ((retval < 0) || (status_buf[0] != CY_START_OF_PACKET)) {
			dev_err(ts->dev,
			"%s: Error getting status r=%d"
				" status_buf[0]=%02X\n",
				__func__, retval, status_buf[0]);
			if (!(retval < 0))
				retval = -EIO;
			goto _cyttsp4_send_cmd_exit;
		} else {
			if (status_buf[CY_STATUS_BYTE] != ERROR_SUCCESS) {
				dev_err(ts->dev,
			"%s: Status=0x%02X error\n",
					__func__, status_buf[CY_STATUS_BYTE]);
				retval = -EIO;
			} else if (stat_ret != NULL) {
				if (num_stat_byte < status_size)
					*stat_ret = status_buf[num_stat_byte];
				else
					*stat_ret = 0;
			}
		}
	} else {
		if (stat_ret != NULL)
			*stat_ret = ERROR_SUCCESS;
		mutex_lock(&ts->data_lock);
	}

_cyttsp4_send_cmd_exit:
	if (status_buf != NULL)
		kfree(status_buf);
	return retval;
}

struct cyttsp4_dev_id {
	u32 silicon_id;
	u8 rev_id;
	u32 bl_ver;
};

#if defined(CY_AUTO_LOAD_FW) || \
	defined(CY_USE_FORCE_LOAD) || \
	defined(CONFIG_TOUCHSCREEN_DEBUG)
#define CY_CMD_LDR_ENTER				0x38
#define CY_CMD_LDR_ENTER_CMD_SIZE			7
#define CY_CMD_LDR_ENTER_STAT_SIZE			15
#define CY_CMD_LDR_INIT					0x48
#define CY_CMD_LDR_INIT_CMD_SIZE			15
#define CY_CMD_LDR_INIT_STAT_SIZE			7
#define CY_CMD_LDR_ERASE_ROW				0x34
#define CY_CMD_LDR_ERASE_ROW_CMD_SIZE			10
#define CY_CMD_LDR_ERASE_ROW_STAT_SIZE			7
#define CY_CMD_LDR_SEND_DATA				0x37
#define CY_CMD_LDR_SEND_DATA_CMD_SIZE			4 /* hdr bytes only */
#define CY_CMD_LDR_SEND_DATA_STAT_SIZE			8
#define CY_CMD_LDR_PROG_ROW				0x39
#define CY_CMD_LDR_PROG_ROW_CMD_SIZE			7 /* hdr bytes only */
#define CY_CMD_LDR_PROG_ROW_STAT_SIZE			7
#define CY_CMD_LDR_VERIFY_ROW				0x3A
#define CY_CMD_LDR_VERIFY_ROW_STAT_SIZE			8
#define CY_CMD_LDR_VERIFY_ROW_CMD_SIZE			10
#define CY_CMD_LDR_VERIFY_CHKSUM			0x31
#define CY_CMD_LDR_VERIFY_CHKSUM_CMD_SIZE		7
#define CY_CMD_LDR_VERIFY_CHKSUM_STAT_SIZE		8

#ifdef CONFIG_TOUCHSCREEN_DEBUG
static const char * const ldr_status_string[] = {
	/* Order must match enum ldr_status above */
	"Error Success",
	"Error Command",
	"Error Flash Array",
	"Error Packet Data",
	"Error Packet Length",
	"Error Packet Checksum",
	"Error Flash Protection",
	"Error Flash Checksum",
	"Error Verify Image",
	"Error Invalid Command",
	"Error Invalid Command",
	"Error Invalid Command",
	"Error Invalid Command",
	"Error Invalid Command",
	"Error Invalid Command",
	"Error Invalid Command",
	"Invalid Error Code"
};

static void _cyttsp4_pr_status(struct cyttsp4 *ts, int status)
	{
	if (status > ERROR_INVALID)
		status = ERROR_INVALID;
	dev_vdbg(ts->dev,
			"%s: status error(%d)=%s\n",
	 __func__, status, ldr_status_string[status]);
}
#endif /* --CONFIG_TOUCHSCREEN_DEBUG */

static u16 _cyttsp4_get_short(u8 *buf)
{
	return ((u16)(*buf) << 8) + *(buf+1);
}

static u8 *_cyttsp4_get_row(struct cyttsp4 *ts,
	u8 *row_buf, u8 *image_buf, int size)
{
	int i;
	for (i = 0; i < size; i++) {
		/* copy a row from the image */
		row_buf[i] = image_buf[i];
	}

	image_buf = image_buf + size;
	return image_buf;
}

static int _cyttsp4_ldr_enter(struct cyttsp4 *ts, struct cyttsp4_dev_id *dev_id)
{
	u16 crc;
	int i = 0;
	size_t cmd_size;
	u8 status_buf[CY_MAX_STATUS_SIZE];
	u8 status = 0;
	int retval = 0;
	/* +1 for TMA400 host sync byte */
	u8 ldr_enter_cmd[CY_CMD_LDR_ENTER_CMD_SIZE+1];

	memset(status_buf, 0, sizeof(status_buf));
	dev_id->bl_ver = 0;
	dev_id->rev_id = 0;
	dev_id->silicon_id = 0;

	ldr_enter_cmd[i++] = CY_START_OF_PACKET;
	ldr_enter_cmd[i++] = CY_CMD_LDR_ENTER;
	ldr_enter_cmd[i++] = 0x00;	/* data len lsb */
	ldr_enter_cmd[i++] = 0x00;	/* data len msb */
#ifdef CY_USE_TMA884
	crc = _cyttsp4_compute_crc(ts, ldr_enter_cmd, i);
	cmd_size = sizeof(ldr_enter_cmd) - 1;
#endif /* --CY_USE_TMA884 */
	ldr_enter_cmd[i++] = (u8)crc;
	ldr_enter_cmd[i++] = (u8)(crc >> 8);
	ldr_enter_cmd[i++] = CY_END_OF_PACKET;

	mutex_unlock(&ts->data_lock);
	INIT_COMPLETION(ts->int_running);
	retval = _cyttsp4_write_block_data(ts, CY_REG_BASE, cmd_size,
		ldr_enter_cmd, ts->platform_data->addr[CY_LDR_ADDR_OFS],
		false);

	if (retval < 0) {
		dev_err(ts->dev,
			"%s: write block failed %d\n", __func__, retval);
		goto _cyttsp4_ldr_enter_exit;
	}

	/* Wait for ISR, get status and lock mutex */
	retval = _cyttsp4_get_status(ts, status_buf,
		CY_CMD_LDR_ENTER_STAT_SIZE, CY_HALF_SEC_TMO_MS);

	if (retval < 0) {
		dev_err(ts->dev,
			"%s: Fail get status to Enter Loader command r=%d\n",
			__func__, retval);
	} else {
		status = status_buf[CY_STATUS_BYTE];
		if (status == ERROR_SUCCESS) {
			dev_id->bl_ver =
				status_buf[11] << 16 |
				status_buf[10] <<  8 |
				status_buf[9] <<  0;
			dev_id->rev_id =
				status_buf[8] <<  0;
			dev_id->silicon_id =
				status_buf[7] << 24 |
				status_buf[6] << 16 |
				status_buf[5] <<  8 |
				status_buf[4] <<  0;
			retval = 0;
		} else
			retval = -EIO;
#ifdef CONFIG_TOUCHSCREEN_DEBUG
			_cyttsp4_pr_status(ts, status);
#endif /* --CONFIG_TOUCHSCREEN_DEBUG */
		dev_vdbg(ts->dev,
			"%s: status=%d "
			"bl_ver=%08X rev_id=%02X silicon_id=%08X\n",
			__func__, status,
			dev_id->bl_ver, dev_id->rev_id, dev_id->silicon_id);
	}

_cyttsp4_ldr_enter_exit:
	return retval;
}


struct cyttsp4_hex_image {
	u8 array_id;
	u16 row_num;
	u16 row_size;
	u8 row_data[CY_DATA_ROW_SIZE];
} __packed;

#ifdef CY_USE_TMA884
static int _cyttsp4_ldr_erase_row(struct cyttsp4 *ts,
	struct cyttsp4_hex_image *row_image)
{
	u16 crc;
	int i = 0;
	int retval = 0;
	/* +1 for TMA400 host sync byte */
	u8 ldr_erase_row_cmd[CY_CMD_LDR_ERASE_ROW_CMD_SIZE+1];

	ldr_erase_row_cmd[i++] = CY_START_OF_PACKET;
	ldr_erase_row_cmd[i++] = CY_CMD_LDR_ERASE_ROW;
	ldr_erase_row_cmd[i++] = 0x03;	/* data len lsb */
	ldr_erase_row_cmd[i++] = 0x00;	/* data len msb */
	ldr_erase_row_cmd[i++] = row_image->array_id;
	ldr_erase_row_cmd[i++] = (u8)row_image->row_num;
	ldr_erase_row_cmd[i++] = (u8)(row_image->row_num >> 8);
#ifdef CY_USE_TMA884
	crc = _cyttsp4_compute_crc(ts, ldr_erase_row_cmd, i);
#endif /* --CY_USE_TMA884 */
	ldr_erase_row_cmd[i++] = (u8)crc;
	ldr_erase_row_cmd[i++] = (u8)(crc >> 8);
	ldr_erase_row_cmd[i++] = CY_END_OF_PACKET;

	retval = _cyttsp4_send_cmd(ts, ldr_erase_row_cmd, i, NULL, 0,
		CY_CMD_LDR_ERASE_ROW_STAT_SIZE, CY_HALF_SEC_TMO_MS);

	if (retval < 0) {
		dev_err(ts->dev,
			"%s: Fail erase row=%d r=%d\n",
			__func__, row_image->row_num, retval);
	}
	return retval;
}
#endif

static int _cyttsp4_ldr_parse_row(struct cyttsp4 *ts, u8 *row_buf,
	struct cyttsp4_hex_image *row_image)
{
	u16 i, j;
	int retval = 0;

	if (!row_buf) {
		dev_err(ts->dev,
			"%s parse row error - buf is null\n", __func__);
		retval = -EINVAL;
		goto cyttsp4_ldr_parse_row_exit;
	}

	row_image->array_id = row_buf[CY_ARRAY_ID_OFFSET];
	row_image->row_num = _cyttsp4_get_short(&row_buf[CY_ROW_NUM_OFFSET]);
	row_image->row_size = _cyttsp4_get_short(&row_buf[CY_ROW_SIZE_OFFSET]);

	if (row_image->row_size > ARRAY_SIZE(row_image->row_data)) {
		dev_err(ts->dev,
			"%s: row data buffer overflow\n", __func__);
		retval = -EOVERFLOW;
		goto cyttsp4_ldr_parse_row_exit;
	}

	for (i = 0, j = CY_ROW_DATA_OFFSET;
		i < row_image->row_size; i++)
		row_image->row_data[i] = row_buf[j++];

	retval = 0;

cyttsp4_ldr_parse_row_exit:
	return retval;
}

static int _cyttsp4_ldr_prog_row(struct cyttsp4 *ts,
	struct cyttsp4_hex_image *row_image)
{
	u16 crc;
	int next;
	int data;
	int row_data;
	u16 row_sum = 0;
	size_t data_len;
#ifdef CY_USE_TMA884
	int segment;
#endif /* --CY_USE_TMA884 */
	int retval = 0;

	u8 *cmd = kzalloc(CY_MAX_PACKET_LEN, GFP_KERNEL);

	if (cmd != NULL) {
		row_data = 0;
		row_sum = 0;

#ifdef CY_USE_TMA884
		for (segment = 0; segment <
			(CY_DATA_ROW_SIZE/CY_PACKET_DATA_LEN)-1;
			segment++) {
			next = 0;
			cmd[next++] = CY_START_OF_PACKET;
			cmd[next++] = CY_CMD_LDR_SEND_DATA;
			cmd[next++] = (u8)CY_PACKET_DATA_LEN;
			cmd[next++] = (u8)(CY_PACKET_DATA_LEN >> 8);

			for (data = 0;
				data < CY_PACKET_DATA_LEN; data++) {
				cmd[next] = row_image->row_data
					[row_data++];
				row_sum += cmd[next];
				next++;
			}

			crc = _cyttsp4_compute_crc(ts, cmd, next);
			cmd[next++] = (u8)crc;
			cmd[next++] = (u8)(crc >> 8);
			cmd[next++] = CY_END_OF_PACKET;

			retval = _cyttsp4_send_cmd(ts, cmd, next, NULL,
				0, CY_CMD_LDR_SEND_DATA_STAT_SIZE,
				CY_HALF_SEC_TMO_MS);

			if (retval < 0) {
				dev_err(ts->dev,
			"%s: send row=%d segment=%d"
					" fail r=%d\n",
					__func__, row_image->row_num,
					segment, retval);
				goto cyttsp4_ldr_prog_row_exit;
			}
		}
#endif /* --CY_USE_TMA884 */

		next = 0;
		cmd[next++] = CY_START_OF_PACKET;
		cmd[next++] = CY_CMD_LDR_PROG_ROW;
		/*
		 * include array id size and row id size in CY_PACKET_DATA_LEN
		 */
#ifdef CY_USE_TMA884
		data_len = CY_PACKET_DATA_LEN;
#endif /* --CY_USE_TMA884 */
		cmd[next++] = (u8)(data_len+3);
		cmd[next++] = (u8)((data_len+3) >> 8);
		cmd[next++] = row_image->array_id;
		cmd[next++] = (u8)row_image->row_num;
		cmd[next++] = (u8)(row_image->row_num >> 8);

		for (data = 0;
			data < data_len; data++) {
			cmd[next] = row_image->row_data[row_data++];
			row_sum += cmd[next];
			next++;
		}

#ifdef CY_USE_TMA884
		crc = _cyttsp4_compute_crc(ts, cmd, next);
#endif /* --CY_USE_TMA884 */
		cmd[next++] = (u8)crc;
		cmd[next++] = (u8)(crc >> 8);
		cmd[next++] = CY_END_OF_PACKET;

		retval = _cyttsp4_send_cmd(ts, cmd, next, NULL, 0,
			CY_CMD_LDR_PROG_ROW_STAT_SIZE, CY_HALF_SEC_TMO_MS);

		if (retval < 0) {
			dev_err(ts->dev,
			"%s: prog row=%d fail r=%d\n",
				__func__, row_image->row_num, retval);
			goto cyttsp4_ldr_prog_row_exit;
		}

	} else {
		dev_err(ts->dev,
			"%s prog row error - cmd buf is NULL\n", __func__);
		retval = -EIO;
	}

cyttsp4_ldr_prog_row_exit:
	if (cmd != NULL)
		kfree(cmd);
	return retval;
}

static int _cyttsp4_ldr_verify_row(struct cyttsp4 *ts,
	struct cyttsp4_hex_image *row_image)
{
	u16 crc;
	int i = 0;
	u8 verify_checksum;
	int retval = 0;
	/* +1 for TMA400 host sync byte */
	u8 ldr_verify_row_cmd[CY_CMD_LDR_VERIFY_ROW_CMD_SIZE+1];

	ldr_verify_row_cmd[i++] = CY_START_OF_PACKET;
	ldr_verify_row_cmd[i++] = CY_CMD_LDR_VERIFY_ROW;
	ldr_verify_row_cmd[i++] = 0x03;	/* data len lsb */
	ldr_verify_row_cmd[i++] = 0x00;	/* data len msb */
	ldr_verify_row_cmd[i++] = row_image->array_id;
	ldr_verify_row_cmd[i++] = (u8)row_image->row_num;
	ldr_verify_row_cmd[i++] = (u8)(row_image->row_num >> 8);
#ifdef CY_USE_TMA884
	crc = _cyttsp4_compute_crc(ts, ldr_verify_row_cmd, i);
#endif /* --CY_USE_TMA884 */
	ldr_verify_row_cmd[i++] = (u8)crc;
	ldr_verify_row_cmd[i++] = (u8)(crc >> 8);
	ldr_verify_row_cmd[i++] = CY_END_OF_PACKET;

	retval = _cyttsp4_send_cmd(ts, ldr_verify_row_cmd, i,
		&verify_checksum, 4,
		CY_CMD_LDR_VERIFY_ROW_STAT_SIZE, CY_HALF_SEC_TMO_MS);

	if (retval < 0) {
		dev_err(ts->dev,
			"%s: verify row=%d fail r=%d\n",
			__func__, row_image->row_num, retval);
	}

	return retval;
}

static int _cyttsp4_ldr_verify_chksum(struct cyttsp4 *ts, u8 *app_chksum)
{
	u16 crc;
	int i = 0;
	int retval = 0;
	/* +1 for TMA400 host sync byte */
	u8 ldr_verify_chksum_cmd[CY_CMD_LDR_VERIFY_CHKSUM_CMD_SIZE+1];

	ldr_verify_chksum_cmd[i++] = CY_START_OF_PACKET;
	ldr_verify_chksum_cmd[i++] = CY_CMD_LDR_VERIFY_CHKSUM;
	ldr_verify_chksum_cmd[i++] = 0x00;	/* data len lsb */
	ldr_verify_chksum_cmd[i++] = 0x00;	/* data len msb */
#ifdef CY_USE_TMA884
	crc = _cyttsp4_compute_crc(ts, ldr_verify_chksum_cmd, i);
#endif /* --CY_USE_TMA884 */
	ldr_verify_chksum_cmd[i++] = (u8)crc;
	ldr_verify_chksum_cmd[i++] = (u8)(crc >> 8);
	ldr_verify_chksum_cmd[i++] = CY_END_OF_PACKET;

	retval = _cyttsp4_send_cmd(ts, ldr_verify_chksum_cmd, i,
		app_chksum, 4,
		CY_CMD_LDR_VERIFY_CHKSUM_STAT_SIZE, CY_HALF_SEC_TMO_MS);

	if (retval < 0) {
		dev_err(ts->dev,
			"%s: verify checksum fail r=%d\n",
			__func__, retval);
	}

	return retval;
}

static int _cyttsp4_load_app(struct cyttsp4 *ts, const u8 *fw, int fw_size)
{
	u8 *p;
#ifdef CY_USE_TMA884
	u8 tries;
#endif
	int ret;
	int retval;	/* need separate return value at exit stage */
	struct cyttsp4_dev_id *file_id = NULL;
	struct cyttsp4_dev_id *dev_id = NULL;
	struct cyttsp4_hex_image *row_image = NULL;
	u8 app_chksum;

	u8 *row_buf = NULL;
	size_t image_rec_size;
	size_t row_buf_size = 1024 > CY_MAX_PRBUF_SIZE ?
		1024 : CY_MAX_PRBUF_SIZE;
	int row_count = 0;

#ifdef CY_USE_TMA884
	image_rec_size = sizeof(struct cyttsp4_hex_image);
#endif /* --CY_USE_TMA884 */

	if (!fw_size || (fw_size % image_rec_size != 0)) {
		dev_err(ts->dev,
			"%s: Firmware image is misaligned\n", __func__);
		retval = -EINVAL;
		goto _cyttsp4_load_app_exit;
	}

#ifdef CY_USE_WATCHDOG
	_cyttsp4_stop_wd_timer(ts);
#endif

	dev_info(ts->dev,
			"%s: start load app\n", __func__);

	row_buf = kzalloc(row_buf_size, GFP_KERNEL);
	row_image = kzalloc(sizeof(struct cyttsp4_hex_image), GFP_KERNEL);
	file_id = kzalloc(sizeof(struct cyttsp4_dev_id), GFP_KERNEL);
	dev_id = kzalloc(sizeof(struct cyttsp4_dev_id), GFP_KERNEL);
	if ((row_buf == NULL) || (row_image == NULL) ||
		(file_id == NULL) || (dev_id == NULL)) {
		dev_err(ts->dev,
			"%s: Unable to alloc row buffers(%p %p %p %p)\n",
			__func__, row_buf, row_image, file_id, dev_id);
		retval = -ENOMEM;
		goto _cyttsp4_load_app_error_exit;
	}

	p = (u8 *)fw;
	/* Enter Loader and return Silicon ID and Rev */

	retval = _cyttsp4_reset(ts);
	if (retval < 0) {
		dev_err(ts->dev,
			"%s: Fail reset device r=%d\n", __func__, retval);
		goto _cyttsp4_load_app_exit;
	}
	retval = _cyttsp4_wait_int(ts, CY_TEN_SEC_TMO_MS * 2);
	if (retval < 0) {
		dev_err(ts->dev,
			"%s: Fail waiting for bootloader interrupt\n",
			__func__);
		goto _cyttsp4_load_app_exit;
	}

	_cyttsp4_change_state(ts, CY_BL_STATE);
	dev_info(ts->dev,
			"%s: Send BL Loader Enter\n", __func__);
	retval = _cyttsp4_ldr_enter(ts, dev_id);
	if (retval < 0) {
		dev_err(ts->dev,
			"%s: Error cannot start Loader (ret=%d)\n",
			__func__, retval);
		goto _cyttsp4_load_app_error_exit;
	}

	dev_vdbg(ts->dev,
		"%s: dev: silicon id=%08X rev=%02X bl=%08X\n",
		__func__, dev_id->silicon_id,
		dev_id->rev_id, dev_id->bl_ver);

	dev_info(ts->dev,
			"%s: Send BL Loader Blocks\n", __func__);
	while (p < (fw + fw_size)) {
		/* Get row */
		dev_dbg(ts->dev,
			"%s: read row=%d\n", __func__, ++row_count);
		memset(row_buf, 0, row_buf_size);
		p = _cyttsp4_get_row(ts, row_buf, p, image_rec_size);

		/* Parse row */
		dev_vdbg(ts->dev,
			"%s: p=%p buf=%p buf[0]=%02X\n", __func__,
			p, row_buf, row_buf[0]);
		retval = _cyttsp4_ldr_parse_row(ts, row_buf, row_image);
		dev_vdbg(ts->dev,
			"%s: array_id=%02X row_num=%04X(%d)"
				" row_size=%04X(%d)\n", __func__,
			row_image->array_id,
			row_image->row_num, row_image->row_num,
			row_image->row_size, row_image->row_size);
		if (retval < 0) {
			dev_err(ts->dev,
			"%s: Parse Row Error "
				"(a=%d r=%d ret=%d\n",
				__func__, row_image->array_id,
				row_image->row_num,
				retval);
			goto bl_exit;
		} else {
			dev_vdbg(ts->dev,
				"%s: Parse Row "
				"(a=%d r=%d ret=%d\n",
				__func__, row_image->array_id,
				row_image->row_num, retval);
		}

#ifdef CY_USE_TMA884
		/* erase row */
		tries = 0;
		do {
			retval = _cyttsp4_ldr_erase_row(ts, row_image);
			if (retval < 0) {
				dev_err(ts->dev,
			"%s: Erase Row Error "
					"(array=%d row=%d ret=%d try=%d)\n",
					__func__, row_image->array_id,
					row_image->row_num, retval, tries);
			}
		} while (retval && tries++ < 5);

		if (retval < 0)
			goto _cyttsp4_load_app_error_exit;
#endif

		/* program row */
		retval = _cyttsp4_ldr_prog_row(ts, row_image);
		if (retval < 0) {
			dev_err(ts->dev,
			"%s: Program Row Error "
				"(array=%d row=%d ret=%d)\n",
				__func__, row_image->array_id,
				row_image->row_num, retval);
			goto _cyttsp4_load_app_error_exit;
		}

		/* verify row */
		retval = _cyttsp4_ldr_verify_row(ts, row_image);
		if (retval < 0) {
			dev_err(ts->dev,
			"%s: Verify Row Error "
				"(array=%d row=%d ret=%d)\n",
				__func__, row_image->array_id,
				row_image->row_num, retval);
			goto _cyttsp4_load_app_error_exit;
		}

		dev_vdbg(ts->dev,
			"%s: array=%d row_cnt=%d row_num=%04X\n",
			__func__, row_image->array_id, row_count,
			row_image->row_num);
	}

	/* verify app checksum */
	retval = _cyttsp4_ldr_verify_chksum(ts, &app_chksum);
	dev_dbg(ts->dev,
		"%s: Application Checksum = %02X r=%d\n",
		__func__, app_chksum, retval);
	if (retval < 0) {
		dev_err(ts->dev,
			"%s: ldr_verify_chksum fail r=%d\n", __func__, retval);
		retval = 0;
	}

	/* exit loader */
bl_exit:
	dev_info(ts->dev,
			"%s: Send BL Loader Terminate\n", __func__);
	ret = _cyttsp4_ldr_exit(ts);
	if (ret) {
		dev_err(ts->dev,
			"%s: Error on exit Loader (ret=%d)\n",
			__func__, ret);
		retval = ret;
		goto _cyttsp4_load_app_error_exit;
	}

	/*
	 * this is a temporary parking state;
	 * the driver will always run startup
	 * after the loader has completed
	 */
	_cyttsp4_change_state(ts, CY_TRANSFER_STATE);
	goto _cyttsp4_load_app_exit;

_cyttsp4_load_app_error_exit:
	_cyttsp4_change_state(ts, CY_BL_STATE);
_cyttsp4_load_app_exit:
	kfree(row_buf);
	kfree(row_image);
	kfree(file_id);
	kfree(dev_id);
	return retval;
}
#endif /* CY_AUTO_LOAD_FW || CY_USE_FORCE_LOAD || CONFIG_TOUCHSCREEN_DEBUG */

/* Constructs loader exit command and sends via _cyttsp4_send_cmd() */
static int _cyttsp4_ldr_exit(struct cyttsp4 *ts)
{
	u16 crc;
	int i = 0;
	int retval = 0;
	/* +1 for TMA400 host sync byte */
	u8 ldr_exit_cmd[CY_CMD_LDR_EXIT_CMD_SIZE+1];

	ldr_exit_cmd[i++] = CY_START_OF_PACKET;
	ldr_exit_cmd[i++] = CY_CMD_LDR_EXIT;
	ldr_exit_cmd[i++] = 0x00;	/* data len lsb */
	ldr_exit_cmd[i++] = 0x00;	/* data len msb */
#ifdef CY_USE_TMA884
	crc = _cyttsp4_compute_crc(ts, ldr_exit_cmd, i);
#endif /* --CY_USE_TMA884 */
	ldr_exit_cmd[i++] = (u8)crc;
	ldr_exit_cmd[i++] = (u8)(crc >> 8);
	ldr_exit_cmd[i++] = CY_END_OF_PACKET;

	retval = _cyttsp4_send_cmd(ts, ldr_exit_cmd, i, NULL, 0,
		CY_CMD_LDR_EXIT_STAT_SIZE, 0);

	if (retval < 0) {
		dev_err(ts->dev,
			"%s: BL Loader exit fail r=%d\n",
			__func__, retval);
	}

	dev_vdbg(ts->dev,
		"%s: Exit BL Loader r=%d\n", __func__, retval);

	return retval;
}

#if defined(CY_USE_FORCE_LOAD) || defined(CONFIG_TOUCHSCREEN_DEBUG)
/* Force firmware upgrade */
static void cyttsp4_firmware_cont(const struct firmware *fw, void *context)
{
	int retval = 0;
	struct device *dev = context;
	struct cyttsp4 *ts = dev_get_drvdata(dev);
	u8 header_size = 0;

	mutex_lock(&ts->data_lock);

	if (fw == NULL) {
		dev_err(ts->dev,
			"%s: Firmware not found\n", __func__);
		goto cyttsp4_firmware_cont_exit;
	}

	if ((fw->data == NULL) || (fw->size == 0)) {
		dev_err(ts->dev,
			"%s: No firmware received\n", __func__);
		goto cyttsp4_firmware_cont_release_exit;
	}

	header_size = fw->data[0];
	if (header_size >= (fw->size + 1)) {
		dev_err(ts->dev,
			"%s: Firmware format is invalid\n", __func__);
		goto cyttsp4_firmware_cont_release_exit;
	}
	retval = _cyttsp4_load_app(ts, &(fw->data[header_size + 1]),
		fw->size - (header_size + 1));
	if (retval < 0) {
		dev_err(ts->dev,
			"%s: Firmware update failed with error code %d\n",
			__func__, retval);
		_cyttsp4_change_state(ts, CY_IDLE_STATE);
		retval = -EIO;
		goto cyttsp4_firmware_cont_release_exit;
	}

#ifdef CONFIG_TOUCHSCREEN_DEBUG
	ts->debug_upgrade = true;
#endif /* --CONFIG_TOUCHSCREEN_DEBUG */

	retval = _cyttsp4_startup(ts);
	if (retval < 0) {
		dev_err(ts->dev,
			"%s: Failed to restart IC with error code %d\n",
			__func__, retval);
		_cyttsp4_change_state(ts, CY_IDLE_STATE);
	}

cyttsp4_firmware_cont_release_exit:
	release_firmware(fw);

cyttsp4_firmware_cont_exit:
	ts->waiting_for_fw = false;
	mutex_unlock(&ts->data_lock);
	return;
}
static ssize_t cyttsp4_ic_reflash_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	static const char *wait_fw_ld = "Driver is waiting for firmware load\n";
	static const char *no_fw_ld = "No firmware loading in progress\n";
	struct cyttsp4 *ts = dev_get_drvdata(dev);

	if (ts->waiting_for_fw)
		return snprintf(buf, strlen(wait_fw_ld)+1, wait_fw_ld);
	else
		return snprintf(buf, strlen(no_fw_ld)+1, no_fw_ld);
}
static ssize_t cyttsp4_ic_reflash_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	int i;
	int retval = 0;
	struct cyttsp4 *ts = dev_get_drvdata(dev);

	if (ts->waiting_for_fw) {
		dev_err(ts->dev,
			"%s: Driver is already waiting for firmware\n",
			__func__);
		retval = -EALREADY;
		goto cyttsp4_ic_reflash_store_exit;
	}

	/*
	 * must configure FW_LOADER in .config file
	 * CONFIG_HOTPLUG=y
	 * CONFIG_FW_LOADER=y
	 * CONFIG_FIRMWARE_IN_KERNEL=y
	 * CONFIG_EXTRA_FIRMWARE=""
	 * CONFIG_EXTRA_FIRMWARE_DIR=""
	 */

	if (size > CY_BL_FW_NAME_SIZE) {
		dev_err(ts->dev,
			"%s: Filename too long\n", __func__);
		retval = -ENAMETOOLONG;
		goto cyttsp4_ic_reflash_store_exit;
	} else {
		/*
		 * name string must be in alloc() memory
		 * or is lost on context switch
		 * strip off any line feed character(s)
		 * at the end of the buf string
		 */
		for (i = 0; buf[i]; i++) {
			if (buf[i] < ' ')
				ts->fwname[i] = 0;
			else
				ts->fwname[i] = buf[i];
		}
	}

	dev_vdbg(ts->dev,
		"%s: Enabling firmware class loader\n", __func__);

	retval = request_firmware_nowait(THIS_MODULE,
		FW_ACTION_NOHOTPLUG, (const char *)ts->fwname, ts->dev,
		GFP_KERNEL, ts->dev, cyttsp4_firmware_cont);
	if (retval < 0) {
		dev_err(ts->dev,
			"%s: Fail request firmware class file load\n",
			__func__);
		ts->waiting_for_fw = false;
		goto cyttsp4_ic_reflash_store_exit;
	} else {
		ts->waiting_for_fw = true;
		retval = size;
	}

cyttsp4_ic_reflash_store_exit:
	return retval;
}
static DEVICE_ATTR(ic_reflash, S_IRUSR | S_IWUSR,
	cyttsp4_ic_reflash_show, cyttsp4_ic_reflash_store);
#endif /* CY_USE_FORCE_LOAD || CONFIG_TOUCHSCREEN_DEBUG */

#ifdef CONFIG_MACH_OMAP4_BOWSER_SUBTYPE_JEM_FTM
static int _cyttsp4_signal_test(struct cyttsp4 *ts)
{
	struct cyttsp4_catdata cat_data;
	int i, j, retval;
	u8 cmd, cmd2[5];
	u8 status[2];
	int row, column, count = 0;
	int retry = 1;

	memset(&cat_data, 0x0, sizeof(cat_data));

	// Get Row, Column parameter
	row = ts->platform_data->sett[CY_IC_GRPNUM_TCH_PARM_VAL]->data[2];
	column = ts->platform_data->sett[CY_IC_GRPNUM_TCH_PARM_VAL]->data[3];
	ftm_test_total_points = row * column;
	printk("Row: 0x%2X , Column: 0x%2X\n", row, column);

	// w 67 02 0B
	cmd = 0x0B;
	retval = _cyttsp4_write_block_data(ts, CY_REG_BASE + offsetof(struct cyttsp4_catdata, cmd),
		sizeof(cmd), &cmd, ts->platform_data->addr[CY_TCH_ADDR_OFS], true);
	if(retval < 0){
		printk("%s: Fail write command 0x%X ret= %d\n", __func__, cmd, retval);
		return -1;
	}

	// delay 50 ms
	for(i = 0 ; i < 50; i++)
		udelay(1000);

	// r 67 x x
	retval = _cyttsp4_read_block_data(ts, CY_REG_BASE + offsetof(struct cyttsp4_catdata, cmd),
		sizeof(status), status, ts->platform_data->addr[CY_TCH_ADDR_OFS], true);
	printk("%s: status[0]:0x%X status[1]: 0x%X\n", __func__, status[0], status[1]);
	if(retval < 0){
		printk("%s: Fail read command 0x%X ret= %d\n", __func__, cmd, retval);
		return -1;
	}

	// w 67 03 00 00 3C C3 02
	for(i = 0 ; i < ftm_test_total_points/247 + 1; ) {
		cmd2[0] = 0x0 + (i * 247) / 256;
		cmd2[1] = 0x0 + (i * 247) % 256;
		cmd2[2] = 0x03;
		cmd2[3] = 0x3C;
		cmd2[4] = 0x2;
		printk("cmd2[0]: 0x%2X , cmd2[1]: 0x%2X, cmd2[2]: 0x%2X , cmd2[3]: 0x%2X\n", cmd2[0], cmd2[1], cmd2[2], cmd2[3]);
		_cyttsp4_write_block_data(ts, CY_REG_BASE + offsetof(struct cyttsp4_catdata, data),
			sizeof(cmd2), cmd2, ts->platform_data->addr[CY_TCH_ADDR_OFS], true);

		_cyttsp4_read_block_data(ts, CY_REG_BASE + offsetof(struct cyttsp4_catdata, cmd),
			CY_NUM_CAT_DATA + 1, &cat_data.cmd, ts->platform_data->addr[CY_TCH_ADDR_OFS], true);

		// w 67 02 0C
		cmd = 0x0C;
		_cyttsp4_write_block_data(ts, CY_REG_BASE + offsetof(struct cyttsp4_catdata, cmd),
			sizeof(cmd), &cmd, ts->platform_data->addr[CY_TCH_ADDR_OFS], true);

		// Use count to prevent infinite loop
		count = 0;
		do {
			// delay 10 ms
			/* According to spec, only delay 10ms, actully we need 200 to 300 ms */
			for(j = 0 ; j < 10; j++)
				udelay(1000);

			// r 67 x*252
			memset(&cat_data, 0xFF, sizeof(cat_data));
			_cyttsp4_read_block_data(ts, CY_REG_BASE + offsetof(struct cyttsp4_catdata, cmd),
				CY_NUM_CAT_DATA + 1, &cat_data.cmd, ts->platform_data->addr[CY_TCH_ADDR_OFS], true);
			count++;
			if(count > 100) {
				printk("[%s] cannot get correct size of signal test\n", __func__);
				return -1;
			}
		} while((cat_data.data[2] << 8) + cat_data.data[3] == ftm_test_total_points);

		if(retry > 0)
		{
			retry = 0;
			i = 0;
			continue;
		}

		for(j = 0 ; (j < 247) && ((j + i *247) < ftm_test_total_points) ; j++) {
			ftm_test_signal_data[j + i*247] = cat_data.data[5+j];
		}
		i++;
	}
	return 0;
}

static ssize_t cyttsp4_ftm_test_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	int i;
	char temp[10];
	sprintf(buf, "data=%d,", ftm_test_total_points);

	for(i = 0 ; i < ftm_test_total_points ; i++)
	{
		memset(temp, '\0', sizeof(temp));
		if(i < ftm_test_total_points-1)
			sprintf(temp, "%d,", ftm_test_signal_data[i]);
		else
			sprintf(temp, "%d", ftm_test_signal_data[i]);
		strcat(buf, temp);
	}

	return strlen(buf)+1;
}

static ssize_t cyttsp4_ftm_test_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	struct cyttsp4 *ts = dev_get_drvdata(dev);
	char messages[256] = {'\0'};
	int retval;
	unsigned long irq_flags = 0;

	strncpy(messages, buf, size);

	if(strncmp(messages, "stop_irq", 8) == 0) {
		printk(KERN_INFO "[%s] to free irq\n", __func__);
		free_irq(ts->irq, ts);
	}
	if(strncmp(messages, "start_irq", 9) == 0) {
#ifdef CY_USE_LEVEL_IRQ
		irq_flags = IRQF_TRIGGER_LOW | IRQF_ONESHOT;
#else
		irq_flags = IRQF_TRIGGER_FALLING | IRQF_ONESHOT;
#endif
		retval = request_threaded_irq(ts->irq, NULL, cyttsp4_irq, irq_flags, ts->input->name, ts);
		if(retval < 0) {
			printk(KERN_INFO "[%s] fail to request irq\n", __func__);
		}
	} else if(strncmp(messages, "reset", 5) == 0) {
		printk(KERN_INFO "[%s] to reset touch\n", __func__);
		retval = _cyttsp4_reset(ts);
		if(retval < 0) {
			printk(KERN_INFO "[%s] fail to reset touch\n", __func__);
		}
	} else if(strncmp(messages, "exit_bootloader", 15) == 0) {
		printk(KERN_INFO "[%s] touch exit bootloader\n", __func__);
		retval = _cyttsp4_ldr_exit(ts);
		if(retval < 0) {
			printk(KERN_INFO "[%s] fail to exit bootloader\n", __func__);
		}
	} else if(strncmp(messages, "operating", 9) == 0) {
		printk(KERN_INFO "[%s] enter operating mode\n", __func__);
		mutex_lock(&(ts->data_lock));
		retval = _cyttsp4_set_mode(ts, CY_OPERATE_MODE);
		mutex_unlock(&(ts->data_lock));
	} else if(strncmp(messages, "testmode", 8) == 0) {
		printk(KERN_INFO "[%s] enter test mode\n", __func__);
		mutex_lock(&(ts->data_lock));
		retval = _cyttsp4_set_mode(ts, CY_CONFIG_MODE);
		mutex_unlock(&(ts->data_lock));
	} else if(strncmp(messages, "signal_test", 11) == 0) {
		printk(KERN_INFO "[%s] enter signal test\n", __func__);
		retval = _cyttsp4_signal_test(ts);
	}

	return size;
}
static DEVICE_ATTR(ftm_test, S_IRUSR | S_IWUSR,
	cyttsp4_ftm_test_show, cyttsp4_ftm_test_store);
#endif

#ifdef CY_USE_TMA884
static int _cyttsp4_calc_data_crc(struct cyttsp4 *ts, size_t ndata, u8 *pdata,
	u8 *crc_h, u8 *crc_l, const char *name)
{
	int retval = 0;
	u8 *buf = NULL;

	*crc_h = 0;
	*crc_l = 0;

	buf = kzalloc(sizeof(uint8_t) * 126, GFP_KERNEL);
	if (buf == NULL) {
		dev_err(ts->dev,
			"%s: Failed to allocate buf\n", __func__);
		retval = -ENOMEM;
		goto _cyttsp4_calc_data_crc_exit;
	}

	if (pdata == NULL) {
		dev_err(ts->dev,
			"%s: bad data pointer\n", __func__);
		retval = -ENXIO;
		goto _cyttsp4_calc_data_crc_exit;
	}

	if (ndata > 122) {
		dev_err(ts->dev,
			"%s: %s is too large n=%d size=%d\n",
			__func__, name, ndata, 126);
		retval = -EOVERFLOW;
		goto _cyttsp4_calc_data_crc_exit;
	}

	buf[0] = 0x00; /* num of config bytes + 4 high */
	buf[1] = 0x7E; /* num of config bytes + 4 low */
	buf[2] = 0x00; /* max block size w/o crc high */
	buf[3] = 0x7E; /* max block size w/o crc low */

	/* Copy platform data */
	memcpy(&(buf[4]), pdata, ndata);

	/* Calculate CRC */
	_cyttsp4_calc_crc(ts, buf, 126, crc_h, crc_l);

	dev_vdbg(ts->dev,
		"%s: crc=%02X%02X\n", __func__, *crc_h, *crc_l);

_cyttsp4_calc_data_crc_exit:
	kfree(buf);
	return retval;
}
#endif /* --CY_USE_TMA884 */


#ifdef CY_USE_TMA884
#ifdef CY_AUTO_LOAD_TOUCH_PARAMS
static int _cyttsp4_calc_settings_crc(struct cyttsp4 *ts, u8 *crc_h, u8 *crc_l)
{
	int retval = 0;
	u8 *buf = NULL;
	u8 size = 0;

	buf = kzalloc(sizeof(uint8_t) * 126, GFP_KERNEL);
	if (buf == NULL) {
		dev_err(ts->dev,
			"%s: Failed to allocate buf\n", __func__);
		retval = -ENOMEM;
		goto _cyttsp4_calc_settings_crc_exit;
	}

	if (ts->platform_data->sett[CY_IC_GRPNUM_TCH_PARM_VAL] == NULL) {
		dev_err(ts->dev,
			"%s: Missing Platform Touch Parameter"
			" values table\n",  __func__);
		retval = -ENXIO;
		goto _cyttsp4_calc_settings_crc_exit;
	}
	if ((ts->platform_data->sett
		[CY_IC_GRPNUM_TCH_PARM_VAL]->data == NULL) ||
		(ts->platform_data->sett
		[CY_IC_GRPNUM_TCH_PARM_VAL]->size == 0)) {
		dev_err(ts->dev,
			"%s: Missing Platform Touch Parameter"
			" values table data\n", __func__);
		retval = -ENXIO;
		goto _cyttsp4_calc_settings_crc_exit;
	}

	size = ts->platform_data->sett[CY_IC_GRPNUM_TCH_PARM_VAL]->size;

	if (size > 122) {
		dev_err(ts->dev,
			"%s: Platform data is too large\n", __func__);
		retval = -EOVERFLOW;
		goto _cyttsp4_calc_settings_crc_exit;
	}

	buf[0] = 0x00; /* num of config bytes + 4 high */
	buf[1] = 0x7E; /* num of config bytes + 4 low */
	buf[2] = 0x00; /* max block size w/o crc high */
	buf[3] = 0x7E; /* max block size w/o crc low */

	/* Copy platform data */
	memcpy(&(buf[4]),
		ts->platform_data->sett[CY_IC_GRPNUM_TCH_PARM_VAL]->data,
		size);

	/* Calculate CRC */
	_cyttsp4_calc_crc(ts, buf, 126, crc_h, crc_l);

_cyttsp4_calc_settings_crc_exit:
	kfree(buf);
	return retval;
}
#endif /* --CY_AUTO_LOAD_TOUCH_PARAMS */
#endif /* --CY_USE_TMA884 */

/* Get IC CRC is operational mode command */
static int _cyttsp4_get_ic_crc(struct cyttsp4 *ts,
	enum cyttsp4_ic_ebid ebid, u8 *crc_h, u8 *crc_l)
{
	int retval = 0;
	u8 cmd_dat[CY_NUM_DAT + 1];	/* +1 for cmd byte */

	memset(cmd_dat, 0, sizeof(cmd_dat));
	cmd_dat[0] = CY_GET_CFG_BLK_CRC;/* pack cmd */
	cmd_dat[1] = ebid;		/* pack EBID id */

	retval = _cyttsp4_put_cmd_wait(ts, ts->si_ofs.cmd_ofs,
		sizeof(cmd_dat), cmd_dat, CY_ONE_SEC_TMO_MS,
		_cyttsp4_chk_cmd_rdy, NULL,
		ts->platform_data->addr[CY_TCH_ADDR_OFS], true, CY_CMD_STATE);
	if (retval < 0) {
		dev_err(ts->dev,
			"%s: Fail Get CRC command r=%d\n",
			__func__, retval);
		goto _cyttsp4_get_ic_crc_exit;
	}

	memset(cmd_dat, 0, sizeof(cmd_dat));
	retval = _cyttsp4_read_block_data(ts, ts->si_ofs.cmd_ofs,
		sizeof(cmd_dat), cmd_dat,
		ts->platform_data->addr[CY_TCH_ADDR_OFS], true);
	if (retval < 0) {
		dev_err(ts->dev,
			"%s: Fail Get CRC status r=%d\n",
			__func__, retval);
		goto _cyttsp4_get_ic_crc_exit;
	}

	/* Check CRC status and assign values */
	if (cmd_dat[1] != 0) {
		dev_err(ts->dev,
			"%s: Get CRC status=%d error\n",
			__func__, cmd_dat[1]);
		retval = -EIO;
		goto _cyttsp4_get_ic_crc_exit;
	}

	*crc_h = cmd_dat[2];
	*crc_l = cmd_dat[3];

_cyttsp4_get_ic_crc_exit:
	return retval;
}

static void _cyttsp4_file_init(struct cyttsp4 *ts)
{
	if (device_create_file(ts->dev, &dev_attr_drv_stat))
		dev_err(ts->dev,
			"%s: Error, could not create drv_stat\n", __func__);

	if (device_create_file(ts->dev, &dev_attr_drv_ver))
		dev_err(ts->dev,
			"%s: Error, could not create drv_ver\n", __func__);

	if (device_create_file(ts->dev, &dev_attr_charger_hdmi))
		dev_err(ts->dev,
			"%s: Error, could not create charger\n", __func__);

#if defined(CY_USE_FORCE_LOAD) || defined(CONFIG_TOUCHSCREEN_DEBUG)
	if (device_create_file(ts->dev, &dev_attr_ic_reflash))
		dev_err(ts->dev,
			"%s: Error, could not create ic_reflash\n", __func__);
#endif

#ifdef CONFIG_TOUCHSCREEN_DEBUG
	if (device_create_file(ts->dev, &dev_attr_hw_reset))
		dev_err(ts->dev,
			"%s: Error, could not create hw_reset\n", __func__);

	if (device_create_file(ts->dev, &dev_attr_hw_recov))
		dev_err(ts->dev,
			"%s: Error, could not create hw_recov\n", __func__);

	if (device_create_file(ts->dev, &dev_attr_ic_grpdata))
		dev_err(ts->dev,
				"%s: Error, could not create ic_grpdata\n", __func__);

	if (device_create_file(ts->dev, &dev_attr_ic_grpnum))
		dev_err(ts->dev,
				"%s: Error, could not create ic_grpnum\n", __func__);

	if (device_create_file(ts->dev, &dev_attr_ic_grpoffset))
		dev_err(ts->dev,
				"%s: Error, could not create ic_grpoffset\n", __func__);

#endif /* --CONFIG_TOUCHSCREEN_DEBUG */


	if (device_create_file(ts->dev, &dev_attr_ic_ver))
		dev_err(ts->dev,
			"%s: Cannot create ic_ver\n", __func__);

	if (device_create_file(ts->dev, &dev_attr_ic_ver_raw))
		dev_err(ts->dev,
			"%s: Cannot create ic_ver_raw\n", __func__);

#ifdef CY_USE_REG_ACCESS
	if (device_create_file(ts->dev, &dev_attr_drv_rw_regid))
		dev_err(ts->dev,
			"%s: Cannot create drv_rw_regid\n", __func__);

	if (device_create_file(ts->dev, &dev_attr_drv_rw_reg_data))
		dev_err(ts->dev,
			"%s: Cannot create drv_rw_reg_data\n", __func__);
#endif
#ifdef CONFIG_MACH_OMAP4_BOWSER_SUBTYPE_JEM_FTM
	if (device_create_file(ts->dev, &dev_attr_ftm_test))
		pr_err("%s: Cannot create ftm_test\n", __func__);
#endif
#ifdef CONFIG_TOUCHSCREEN_DEBUG_ENABLE_ENTRY
	if (device_create_file(ts->dev, &dev_attr_ts_debug))
		dev_err(ts->dev,
			"%s: Cannot create ts_debug\n", __func__);
#endif

	ts->sysfs_files_created = true;

	return;
}

static void _cyttsp4_file_free(struct input_dev *dev)
{
	device_remove_file(dev, &dev_attr_drv_ver);
	device_remove_file(dev, &dev_attr_drv_stat);
	device_remove_file(dev, &dev_attr_ic_ver);
	device_remove_file(dev, &dev_attr_ic_ver_raw);
#if defined(CY_USE_FORCE_LOAD) || defined(CONFIG_TOUCHSCREEN_DEBUG)
	device_remove_file(dev, &dev_attr_ic_reflash);
#endif

#ifdef CONFIG_TOUCHSCREEN_DEBUG
	device_remove_file(dev, &dev_attr_ic_grpnum);
	device_remove_file(dev, &dev_attr_ic_grpoffset);
	device_remove_file(dev, &dev_attr_ic_grpdata);
	device_remove_file(dev, &dev_attr_hw_reset);
	device_remove_file(dev, &dev_attr_hw_recov);
#endif /* --CONFIG_TOUCHSCREEN_DEBUG */

#ifdef CY_USE_REG_ACCESS
	device_remove_file(dev, &dev_attr_drv_rw_regid);
	device_remove_file(dev, &dev_attr_drv_rw_reg_data);
#endif
#ifdef CONFIG_MACH_OMAP4_BOWSER_SUBTYPE_JEM_FTM
	device_remove_file(dev, &dev_attr_ftm_test);
#endif
#ifdef CONFIG_TOUCHSCREEN_DEBUG_ENABLE_ENTRY
	device_remove_file(dev, &dev_attr_ts_debug);
#endif
}

#ifdef CY_USE_TMA884
#define CY_IRQ_DEASSERT	1
#define CY_IRQ_ASSERT	0
static int _cyttsp4_startup(struct cyttsp4 *ts)
{
	int retval = 0;
	int i = 0;
	u8 pdata_crc[2];
	u8 ic_crc[2];
	bool upgraded = false;
	bool mddata_updated = false;
	bool wrote_sysinfo_regs = false;
	bool wrote_settings = false;

	memset(&ts->test, 0, sizeof(struct cyttsp4_test_mode));

	ts->prev_record_count = 0xFF;

#ifdef CY_USE_WATCHDOG
	_cyttsp4_stop_wd_timer(ts);
#endif
_cyttsp4_startup_start:
	memset(pdata_crc, 0, sizeof(pdata_crc));
	memset(ic_crc, 0, sizeof(ic_crc));
	dev_vdbg(ts->dev,
		"%s: enter driver_state=%d\n", __func__, ts->driver_state);
	_cyttsp4_change_state(ts, CY_BL_STATE);

	retval = _cyttsp4_reset(ts);
	if (retval < 0) {
		dev_err(ts->dev,
			"%s: Fail reset device r=%d\n", __func__, retval);
		/* continue anyway in case device was already in bootloader */
	}

	/* wait for interrupt to set ready completion */
	retval = _cyttsp4_wait_int(ts, CY_HALF_SEC_TMO_MS);
	if (retval < 0) {
		dev_err(ts->dev,
			"%s: Fail waiting for bootloader interrupt\n",
			__func__);
		goto _cyttsp4_startup_exit;
	}

	INIT_COMPLETION(ts->si_int_running);
	_cyttsp4_change_state(ts, CY_EXIT_BL_STATE);
	ts->switch_flag = true;
	retval = _cyttsp4_wait_si_int(ts, CY_TEN_SEC_TMO_MS);
	if (retval < 0) {
		dev_err(ts->dev,
			"%s: Fail wait switch to Sysinfo r=%d\n",
			__func__, retval);
		/* continue anyway in case sync missed */
	}
	if (ts->driver_state != CY_SYSINFO_STATE)
		_cyttsp4_change_state(ts, CY_SYSINFO_STATE);
	else
		_cyttsp4_pr_state(ts);

	/*
	 * TODO: remove this wait for toggle high when
	 * startup from ES10 firmware is no longer required
	 */
	/* Wait for IRQ to toggle high */
	dev_vdbg(ts->dev,
		"%s: wait for irq toggle high\n", __func__);
	retval = -ETIMEDOUT;
	for (i = 0; i < CY_DELAY_MAX * 10 * 5; i++) {
		if (ts->platform_data->irq_stat() == CY_IRQ_DEASSERT) {
			retval = 0;
			break;
		}
		mdelay(CY_DELAY_DFLT);
	}
	if (retval < 0) {
		dev_err(ts->dev,
			"%s: timeout waiting for irq to de-assert\n",
			__func__);
		goto _cyttsp4_startup_exit;
	}

	dev_vdbg(ts->dev,
		"%s: read sysinfo 1\n", __func__);
	memset(&ts->sysinfo_data, 0,
		sizeof(struct cyttsp4_sysinfo_data));
	retval = _cyttsp4_read_block_data(ts, CY_REG_BASE,
		sizeof(struct cyttsp4_sysinfo_data), &ts->sysinfo_data,
		ts->platform_data->addr[CY_TCH_ADDR_OFS], true);
	if (retval < 0) {
		dev_err(ts->dev,
			"%s: Fail to switch from Bootloader "
			"to Application r=%d\n",
			__func__, retval);

		_cyttsp4_change_state(ts, CY_BL_STATE);

		if (upgraded) {
			dev_err(ts->dev,
			"%s: app failed to launch after"
				" platform firmware upgrade\n", __func__);
			retval = -EIO;
			goto _cyttsp4_startup_exit;
		}

#ifdef CY_AUTO_LOAD_FW
		dev_info(ts->dev,
			"%s: attempting to reflash IC...\n", __func__);
		if (ts->platform_data->fw->img == NULL ||
			ts->platform_data->fw->size == 0) {
			dev_err(ts->dev,
			"%s: no platform firmware available"
				" for reflashing\n", __func__);
			_cyttsp4_change_state(ts, CY_INVALID_STATE);
			retval = -ENODATA;
			goto _cyttsp4_startup_exit;
		}
		retval = _cyttsp4_load_app(ts,
			ts->platform_data->fw->img,
			ts->platform_data->fw->size);
		if (retval) {
			dev_err(ts->dev,
			"%s: failed to reflash IC (r=%d)\n",
				__func__, retval);
			_cyttsp4_change_state(ts, CY_INVALID_STATE);
			retval = -EIO;
			goto _cyttsp4_startup_exit;
		}
		upgraded = true;
		dev_info(ts->dev,
			"%s: resetting IC after reflashing\n", __func__);
		goto _cyttsp4_startup_start; /* Reset the part */
#endif /* --CY_AUTO_LOAD_FW */
	}

	/*
	 * read system information registers
	 * get version numbers and fill sysinfo regs
	 */
	dev_vdbg(ts->dev,
		"%s: Read Sysinfo regs and get version numbers\n", __func__);
	retval = _cyttsp4_get_sysinfo_regs(ts);
	if (retval < 0) {
		dev_err(ts->dev,
			"%s: Read Block fail -get sys regs (r=%d)\n",
			__func__, retval);
		_cyttsp4_change_state(ts, CY_IDLE_STATE);
		goto _cyttsp4_startup_exit;
	}

#ifdef CY_AUTO_LOAD_FW
#ifdef CONFIG_TOUCHSCREEN_DEBUG
	if (!ts->ic_grptest && !(ts->debug_upgrade)) {
#endif /* --CONFIG_TOUCHSCREEN_DEBUG */
		retval = _cyttsp4_boot_loader(ts, &upgraded);
		if (retval < 0) {
			dev_err(ts->dev,
			"%s: fail boot loader r=%d)\n",
				__func__, retval);
			_cyttsp4_change_state(ts, CY_IDLE_STATE);
			goto _cyttsp4_startup_exit;
		}
		if (upgraded)
			goto _cyttsp4_startup_start;
#ifdef CONFIG_TOUCHSCREEN_DEBUG
	}
#endif /* --CONFIG_TOUCHSCREEN_DEBUG */
#endif /* --CY_AUTO_LOAD_FW */

	if (!wrote_sysinfo_regs) {
#ifdef CONFIG_TOUCHSCREEN_DEBUG
		if (ts->ic_grptest)
				goto _cyttsp4_startup_set_sysinfo_done;
#endif /* --CONFIG_TOUCHSCREEN_DEBUG */
		dev_vdbg(ts->dev,
			"%s: Set Sysinfo regs\n", __func__);
		retval = _cyttsp4_set_mode(ts, CY_SYSINFO_MODE);
		if (retval < 0) {
			dev_err(ts->dev,
			"%s: Set SysInfo Mode fail r=%d\n",
				__func__, retval);
			_cyttsp4_change_state(ts, CY_IDLE_STATE);
			goto _cyttsp4_startup_exit;
		}
		retval = _cyttsp4_set_sysinfo_regs(ts, &mddata_updated);
		if (retval < 0) {
			dev_err(ts->dev,
			"%s: Set SysInfo Regs fail r=%d\n",
				__func__, retval);
			_cyttsp4_change_state(ts, CY_IDLE_STATE);
			goto _cyttsp4_startup_exit;
		} else
			wrote_sysinfo_regs = true;
	}
#ifdef CONFIG_TOUCHSCREEN_DEBUG
_cyttsp4_startup_set_sysinfo_done:
#endif /* --CONFIG_TOUCHSCREEN_DEBUG */

	dev_vdbg(ts->dev,
		"%s: enter operational mode\n", __func__);
	retval = _cyttsp4_set_mode(ts, CY_OPERATE_MODE);
	if (retval < 0) {
		_cyttsp4_change_state(ts, CY_IDLE_STATE);
		dev_err(ts->dev,
			"%s: Fail set operational mode (r=%d)\n",
			__func__, retval);
		goto _cyttsp4_startup_exit;
	} else {
#ifdef CY_AUTO_LOAD_TOUCH_PARAMS
#ifdef CONFIG_TOUCHSCREEN_DEBUG
		if (ts->ic_grptest)
			goto _cyttsp4_startup_settings_valid;
#endif

		/* check idme data for whether or not panel is good */
		ts->platform_data->sett[CY_IC_GRPNUM_TCH_PARM_VAL]->data[CY_AFH_OPMODE_INDEX] =
				idme_is_good_panel();

		dev_vdbg(ts->dev,
				"%s: good panel/AFH setting = %d\n",__func__,
				ts->platform_data->sett[CY_IC_GRPNUM_TCH_PARM_VAL]->data[CY_AFH_OPMODE_INDEX]);

		/* Calculate settings CRC from platform settings */
		dev_vdbg(ts->dev,
			"%s: Calculate settings CRC and get IC CRC\n",
			__func__);
		retval = _cyttsp4_calc_settings_crc(ts,
			&pdata_crc[0], &pdata_crc[1]);
		if (retval < 0) {
			dev_err(ts->dev,
			"%s: Unable to calculate settings CRC\n",
				__func__);
			goto _cyttsp4_startup_exit;
		}

		/* Get settings CRC from touch IC */
		retval = _cyttsp4_get_ic_crc(ts, CY_TCH_PARM_EBID,
			&ic_crc[0], &ic_crc[1]);
		if (retval < 0) {
			dev_err(ts->dev,
			"%s: Unable to get settings CRC\n", __func__);
			goto _cyttsp4_startup_exit;
		}

		/* Compare CRC values */
		dev_vdbg(ts->dev,
			"%s: PDATA CRC = 0x%02X%02X, IC CRC = 0x%02X%02X\n",
			__func__, pdata_crc[0], pdata_crc[1],
			ic_crc[0], ic_crc[1]);

		if ((pdata_crc[0] == ic_crc[0]) &&
			(pdata_crc[1] == ic_crc[1]))
			goto _cyttsp4_startup_settings_valid;

		/* Update settings */
		dev_info(ts->dev,
			"%s: Updating IC settings...\n", __func__);

		if (wrote_settings) {
			dev_err(ts->dev,
			"%s: Already updated IC settings\n",
				__func__);
			goto _cyttsp4_startup_settings_valid;
		}

		retval = _cyttsp4_set_op_params(ts, pdata_crc[0], pdata_crc[1]);
		if (retval < 0) {
			dev_err(ts->dev,
			"%s: Set Operational Params fail r=%d\n",
				__func__, retval);
			goto _cyttsp4_startup_exit;
		}

		wrote_settings = true;
#else
		wrote_settings = false;
#endif /* --CY_AUTO_LOAD_TOUCH_PARAMS */
	}

#ifdef CY_AUTO_LOAD_TOUCH_PARAMS
_cyttsp4_startup_settings_valid:
#endif /* --CY_AUTO_LOAD_TOUCH_PARAMS */
	if (mddata_updated || wrote_settings) {
		dev_info(ts->dev,
			"%s: Resetting IC after writing settings %i %i\n",
			__func__, mddata_updated, wrote_settings);
		mddata_updated = false;
		wrote_settings = false;
		goto _cyttsp4_startup_start; /* Reset the part */
	}
	dev_vdbg(ts->dev,
		"%s: enable handshake\n", __func__);
	retval = _cyttsp4_handshake_enable(ts);
	if (retval < 0)
		dev_err(ts->dev,
			"%s: fail enable handshake r=%d", __func__, retval);

	_cyttsp4_change_state(ts, CY_ACTIVE_STATE);

	if (ts->was_suspended) {
		ts->was_suspended = false;
		retval = _cyttsp4_enter_sleep(ts);
		if (retval < 0) {
			dev_err(ts->dev,
			"%s: fail resume sleep r=%d\n",
				__func__, retval);
		}
	} else {
#ifdef CY_USE_WATCHDOG
		_cyttsp4_start_wd_timer(ts);
#endif
	}

	if (!(ts->sysfs_files_created))
	{
		/* add /sys files */
		_cyttsp4_file_init(ts);
	}

_cyttsp4_startup_exit:
	ts->low_power_enable = true;
	return retval;
}
#endif /* --CY_USE_TMA884 */

static irqreturn_t cyttsp4_irq(int irq, void *handle)
{
	struct cyttsp4 *ts = handle;
	u8 rep_stat = 0;
	int retval = 0;

	dev_vdbg(ts->dev,
		"%s: GOT IRQ ps=%d\n", __func__, ts->driver_state);
	mutex_lock(&ts->data_lock);

	dev_vdbg(ts->dev,
		"%s: DO IRQ ps=%d\n", __func__, ts->driver_state);

	switch (ts->driver_state) {
	case CY_BL_STATE:
	case CY_CMD_STATE:
		complete(&ts->int_running);
#ifdef CY_USE_LEVEL_IRQ
		udelay(1000);
#endif
		break;
	case CY_SYSINFO_STATE:
		complete(&ts->si_int_running);
#ifdef CY_USE_LEVEL_IRQ
		udelay(500);
#endif
		break;
	case CY_EXIT_BL_STATE:
#ifdef CY_USE_LEVEL_IRQ
		udelay(1000);
#endif
		if (ts->switch_flag == true) {
			ts->switch_flag = false;
			retval = _cyttsp4_ldr_exit(ts);
			if (retval < 0) {
				dev_err(ts->dev,
			"%s: Fail bl exit r=%d\n",
					__func__, retval);
			} else
				ts->driver_state = CY_SYSINFO_STATE;
		}
		break;
	case CY_SLEEP_STATE:
		dev_info(ts->dev,
			"%s: Attempt to process touch after enter sleep or"
			" unexpected wake event\n", __func__);
		/* can't hold any locks when calling power functions */
		mutex_unlock(&ts->data_lock);
		retval = _cyttsp4_wakeup(ts); /* in case its really asleep */
		if (retval < 0) {
			dev_err(ts->dev,
			"%s: wakeup fail r=%d\n",
				__func__, retval);
			_cyttsp4_pr_state(ts);
			_cyttsp4_queue_startup(ts, true);
			break;
		}
		/* Put the part back to sleep */
		retval = _cyttsp4_enter_sleep(ts);
		if (retval < 0) {
			dev_err(ts->dev,
			"%s: fail resume sleep r=%d\n",
				__func__, retval);
			_cyttsp4_pr_state(ts);
			_cyttsp4_queue_startup(ts, true);
		}
		mutex_lock(&ts->data_lock);
		break;
	case CY_IDLE_STATE:
		if (ts->xy_mode == NULL) {
			/* initialization is not complete; invalid pointers */
			break;
		}

		/* device now available; signal initialization */
		dev_info(ts->dev,
			"%s: Received IRQ in IDLE state\n",
			__func__);
		/* Try to determine the IC's current state */
		retval = _cyttsp4_load_status_regs(ts);
		if (retval < 0) {
			dev_err(ts->dev,
			"%s: Still unable to access IC after IRQ r=%d\n",
				__func__, retval);
			break;
		}
		rep_stat = ts->xy_mode[ts->si_ofs.rep_ofs + 1];
		if (IS_BOOTLOADERMODE(rep_stat)) {
			dev_info(ts->dev,
			"%s: BL mode found in IDLE state\n",
				__func__);
			_cyttsp4_queue_startup(ts, false);
			break;
		}
		dev_err(ts->dev,
			"%s: interrupt received in IDLE state -"
			" try processing touch\n",
			__func__);
		_cyttsp4_change_state(ts, CY_ACTIVE_STATE);
#ifdef CY_USE_WATCHDOG
		_cyttsp4_start_wd_timer(ts);
#endif
		retval = _cyttsp4_xy_worker(ts);
		if (retval < 0) {
			dev_err(ts->dev,
			"%s: xy_worker IDLE fail r=%d\n",
				__func__, retval);
			_cyttsp4_queue_startup(ts, false);
			break;
		}

#ifdef CY_USE_LEVEL_IRQ
		udelay(500);
#endif
		break;
	case CY_READY_STATE:
		complete(&ts->ready_int_running);
		/* do not break; do worker */
	case CY_OPCMD_STATE:
	{
		u8 cmd_status = 0;

		retval = _cyttsp4_read_block_data(ts, ts->si_ofs.cmd_ofs,
				1, &cmd_status, ts->platform_data->addr[CY_TCH_ADDR_OFS], true);
		if (retval < 0)
		{
			dev_err(ts->dev, "%s: unable to read cmd_status in ISR\n", __func__);

		} else if (cmd_status & CY_CMD_RDY_BIT) {
			complete(&ts->int_running);
			break;
		}
		/* else fall through and process touches */
	}
	case CY_ACTIVE_STATE:
		if (ts->test.cur_mode == CY_TEST_MODE_CAT) {
			complete(&ts->int_running);
#ifdef CY_USE_LEVEL_IRQ
			udelay(500);
#endif
		} else {

			/* process the touches */
			retval = _cyttsp4_xy_worker(ts);
			if (retval < 0) {
				dev_err(ts->dev,
			"%s: XY Worker fail r=%d\n",
					__func__, retval);

				/* unlock before queuing startup
				   to prevent blocking execution */
				mutex_unlock(&ts->data_lock);
				_cyttsp4_queue_startup(ts, false);

				/* already unlocked goto exit */
				goto cyttsp4_irq_exit_already_unlocked;

			}
		}
		break;
	default:
		break;
	}

	mutex_unlock(&ts->data_lock);

cyttsp4_irq_exit_already_unlocked:
	dev_vdbg(ts->dev,
		"%s: DONE IRQ ps=%d\n", __func__, ts->driver_state);

	return IRQ_HANDLED;
}



static int cyttsp4_open(struct input_dev *dev)
{
	int retval = 0;

	struct cyttsp4 *ts = input_get_drvdata(dev);
	dev_dbg(ts->dev, "%s: Open call ts=%p\n", __func__, ts);
	mutex_lock(&ts->data_lock);
	if (!ts->powered) {
		/*
		 * execute complete startup procedure.  After this
		 * call the device is in active state and the worker
		 * is running
		 */
		retval = _cyttsp4_startup(ts);

		/* powered if no hard failure */
		if (retval < 0) {
			ts->powered = false;
			_cyttsp4_change_state(ts, CY_IDLE_STATE);
			dev_err(ts->dev,
			"%s: startup fail at power on r=%d\n",
				__func__, retval);
		} else
			ts->powered = true;

		dev_info(ts->dev,
			"%s: Powered ON(%d) r=%d\n",
			__func__, (int)ts->powered, retval);
	}
	mutex_unlock(&ts->data_lock);
	return 0;
}

static void cyttsp4_close(struct input_dev *dev)
{
	/*
	 * close() normally powers down the device
	 * this call simply returns unless power
	 * to the device can be controlled by the driver
	 */
	_cyttsp4_file_free(dev);

	return;
}

void cyttsp4_core_release(void *handle)
{
	struct cyttsp4 *ts = handle;

	dev_dbg(ts->dev, "%s: Release call ts=%p\n",
		__func__, ts);
	if (ts == NULL) {
		dev_err(ts->dev,
			"%s: Null context pointer on driver release\n",
			__func__);
		goto cyttsp4_core_release_exit;
	}

#ifdef CONFIG_HAS_EARLYSUSPEND
	unregister_early_suspend(&ts->early_suspend);
#endif

	if (mutex_is_locked(&ts->data_lock))
		mutex_unlock(&ts->data_lock);
	mutex_destroy(&ts->data_lock);
	free_irq(ts->irq, ts);
	input_unregister_device(ts->input);
	if (ts->cyttsp4_wq) {
		destroy_workqueue(ts->cyttsp4_wq);
		ts->cyttsp4_wq = NULL;
	}

	if (ts->sysinfo_ptr.cydata != NULL)
		kfree(ts->sysinfo_ptr.cydata);
	if (ts->sysinfo_ptr.test != NULL)
		kfree(ts->sysinfo_ptr.test);
	if (ts->sysinfo_ptr.pcfg != NULL)
		kfree(ts->sysinfo_ptr.pcfg);
	if (ts->sysinfo_ptr.opcfg != NULL)
		kfree(ts->sysinfo_ptr.opcfg);
	if (ts->sysinfo_ptr.ddata != NULL)
		kfree(ts->sysinfo_ptr.ddata);
	if (ts->sysinfo_ptr.mdata != NULL)
		kfree(ts->sysinfo_ptr.mdata);
	if (ts->xy_mode != NULL)
		kfree(ts->xy_mode);
	if (ts->xy_data != NULL)
		kfree(ts->xy_data);
	if (ts->xy_data_touch1 != NULL)
		kfree(ts->xy_data_touch1);

	kfree(ts);
cyttsp4_core_release_exit:
	return;
}
EXPORT_SYMBOL_GPL(cyttsp4_core_release);

void *cyttsp4_core_init(struct cyttsp4_bus_ops *bus_ops,
	struct device *dev, int irq, char *name)
{
	unsigned long irq_flags = 0;
	int i = 0;
	int min = 0;
	int max = 0;
	u16 signal = 0;
	int retval = 0;
	struct input_dev *input_device = NULL;
	struct cyttsp4 *ts = NULL;

	if (dev == NULL) {
		pr_err("%s: Error, dev pointer is Null\n", __func__);
		goto error_alloc_data;
	}

	if (bus_ops == NULL) {
		dev_err(dev,
			"%s: Error, bus_ops Pointer is Null\n", __func__);
		goto error_alloc_data;
	}
	ts = kzalloc(sizeof(*ts), GFP_KERNEL);
	if (ts == NULL) {
		dev_err(dev,
			"%s: Error, kzalloc context memory\n", __func__);
		goto error_alloc_data;
	}

#if defined(CY_USE_FORCE_LOAD) || defined(CONFIG_TOUCHSCREEN_DEBUG)
	ts->fwname = kzalloc(CY_BL_FW_NAME_SIZE, GFP_KERNEL);
	if (ts->fwname == NULL) {
		dev_err(dev,
			"%s: Error, kzalloc fwname\n", __func__);
		goto error_alloc_failed;
	}
#endif

#ifdef CONFIG_TOUCHSCREEN_DEBUG
	ts->pr_buf = kzalloc(CY_MAX_PRBUF_SIZE, GFP_KERNEL);
	if (ts->pr_buf == NULL) {
			dev_err(dev,
						"%s: Error, kzalloc pr_buf\n", __func__);
			goto error_alloc_failed;
	}
#endif /* --CONFIG_TOUCHSCREEN_DEBUG */

	ts->cyttsp4_wq =
		create_singlethread_workqueue("cyttsp4_resume_startup_wq");
	if (ts->cyttsp4_wq == NULL) {
		dev_err(dev,
			"%s: No memory for cyttsp4_resume_startup_wq\n",
			__func__);
		goto error_alloc_failed;
	}

	ts->driver_state = CY_INVALID_STATE;
	ts->current_mode = CY_MODE_BOOTLOADER;
	ts->powered = false;
	ts->was_suspended = false;
	ts->switch_flag = false;
	ts->soft_reset_asserted = false;
	ts->num_prv_tch = 0;

	ts->xy_data = NULL;
	ts->xy_mode = NULL;
	ts->xy_data_touch1 = NULL;
	ts->btn_rec_data = NULL;
	memset(&ts->test, 0, sizeof(struct cyttsp4_test_mode));

#ifdef CONFIG_TOUCHSCREEN_DEBUG_ENABLE_ENTRY
	ts->debug_enable = false;
#endif
	ts->low_power_enable = false;
	ts->sysfs_files_created = false;
	ts->charger_hdmi_update_pending = false;
	ts->suspend_blocked = false;
	ts->suspend_in_prog = false;
	ts->resume_in_prog = false;
	ts->dev = dev;
	ts->bus_ops = bus_ops;
	ts->platform_data = dev->platform_data;
	if (ts->platform_data == NULL) {
		dev_err(ts->dev,
			"%s: Error, platform data is Null\n", __func__);
		goto error_alloc_failed;
	}

	if (ts->platform_data->frmwrk == NULL) {
		dev_err(ts->dev,
			"%s: Error, platform data framework is Null\n",
			__func__);
		goto error_alloc_failed;
	}

	if (ts->platform_data->frmwrk->abs == NULL) {
		dev_err(ts->dev,
			"%s: Error, platform data framework array is Null\n",
			__func__);
		goto error_alloc_failed;
	}

	mutex_init(&ts->data_lock);
	mutex_init(&ts->suspend_lock);
	init_completion(&ts->int_running);
	init_completion(&ts->si_int_running);
	init_completion(&ts->ready_int_running);
	ts->flags = ts->platform_data->flags;
#if defined(CY_USE_FORCE_LOAD) || defined(CONFIG_TOUCHSCREEN_DEBUG)
	ts->waiting_for_fw = false;
#endif
#ifdef CONFIG_TOUCHSCREEN_DEBUG
	ts->debug_upgrade = false;
	ts->ic_grpnum = CY_IC_GRPNUM_RESERVED;
	ts->ic_grpoffset = 0;
	ts->ic_grptest = false;
	ts->bus_ops->tsdebug = CY_DBG_LVL_0;
#endif /* --CONFIG_TOUCHSCREEN_DEBUG */

#ifdef CY_USE_TMA884
	ts->max_config_bytes = CY_TMA884_MAX_BYTES;
#endif /* --CY_USE_TMA884 */

	ts->irq = irq;
	if (ts->irq <= 0) {
		dev_vdbg(ts->dev,
			"%s: Error, failed to allocate irq\n", __func__);
			goto error_init;
	}

	/* Create the input device and register it. */
	dev_vdbg(ts->dev,
		"%s: Create the input device and register it\n", __func__);
	input_device = input_allocate_device();
	if (input_device == NULL) {
		dev_err(ts->dev,
			"%s: Error, failed to allocate input device\n",
			__func__);
		goto error_init;
	}

	ts->input = input_device;
	input_device->name = name;
	snprintf(ts->phys, sizeof(ts->phys)-1, "%s", dev_name(dev));
	input_device->phys = ts->phys;
	input_device->dev.parent = ts->dev;
	ts->bus_type = bus_ops->dev->bus;
#ifdef CY_USE_WATCHDOG
	INIT_WORK(&ts->work, cyttsp4_timer_watchdog);
	setup_timer(&ts->timer, cyttsp4_timer, (unsigned long)ts);
#endif

	input_device->open = cyttsp4_open;
	input_device->close = cyttsp4_close;
	input_set_drvdata(input_device, ts);
	dev_set_drvdata(dev, ts);

	dev_vdbg(ts->dev,
		"%s: Initialize event signals\n", __func__);
	__set_bit(EV_ABS, input_device->evbit);
	__set_bit(EV_REL, input_device->evbit);
#ifdef CONFIG_MACH_OMAP4_BOWSER_SUBTYPE_JEM_FTM
	__set_bit(EV_KEY, input_device->evbit);
#endif
	//bitmap_fill(input_device->keybit, KEY_MAX);
	bitmap_fill(input_device->relbit, REL_MAX);
	bitmap_fill(input_device->absbit, ABS_MAX);

	/* ICS touch down button press signal */
	__set_bit(BTN_TOUCH, input_device->keybit);

	for (i = 0; i < (ts->platform_data->frmwrk->size / CY_NUM_ABS_SET);
		i++) {
		signal = ts->platform_data->frmwrk->abs[
			(i * CY_NUM_ABS_SET) + CY_SIGNAL_OST];
		if (signal != CY_IGNORE_VALUE) {
			min = ts->platform_data->frmwrk->abs
				[(i * CY_NUM_ABS_SET) + CY_MIN_OST];
			max = ts->platform_data->frmwrk->abs
				[(i * CY_NUM_ABS_SET) + CY_MAX_OST];
			if (i == CY_ABS_ID_OST) {
				/* shift track ids down to start at 0 */
				max = max - min;
				min = min - min;
			}
			input_set_abs_params(input_device,
				signal,
				min,
				max,
				ts->platform_data->frmwrk->abs[
					(i * CY_NUM_ABS_SET) + CY_FUZZ_OST],
				ts->platform_data->frmwrk->abs[
					(i * CY_NUM_ABS_SET) + CY_FLAT_OST]);
			dev_vdbg(ts->dev,
				"%s: s=%02X min=%d max=%d fuzz=%d flat=%d\n",
				__func__, signal, min, max,
				ts->platform_data->frmwrk->abs[
					(i * CY_NUM_ABS_SET) + CY_FUZZ_OST],
				ts->platform_data->frmwrk->abs[
					(i * CY_NUM_ABS_SET) + CY_FLAT_OST]);

		}
	}

#ifdef CY_USE_DEBUG_TOOLS
	if (ts->flags & CY_FLAG_FLIP) {
		input_set_abs_params(input_device,
			ABS_MT_POSITION_X,
			ts->platform_data->frmwrk->abs
			[(CY_ABS_Y_OST * CY_NUM_ABS_SET) + CY_MIN_OST],
			ts->platform_data->frmwrk->abs
			[(CY_ABS_Y_OST * CY_NUM_ABS_SET) + CY_MAX_OST],
			ts->platform_data->frmwrk->abs
			[(CY_ABS_Y_OST * CY_NUM_ABS_SET) + CY_FUZZ_OST],
			ts->platform_data->frmwrk->abs
			[(CY_ABS_Y_OST * CY_NUM_ABS_SET) + CY_FLAT_OST]);

		input_set_abs_params(input_device,
			ABS_MT_POSITION_Y,
			ts->platform_data->frmwrk->abs
			[(CY_ABS_X_OST * CY_NUM_ABS_SET) + CY_MIN_OST],
			ts->platform_data->frmwrk->abs
			[(CY_ABS_X_OST * CY_NUM_ABS_SET) + CY_MAX_OST],
			ts->platform_data->frmwrk->abs
			[(CY_ABS_X_OST * CY_NUM_ABS_SET) + CY_FUZZ_OST],
			ts->platform_data->frmwrk->abs
			[(CY_ABS_X_OST * CY_NUM_ABS_SET) + CY_FLAT_OST]);
	}
#endif /* --CY_USE_DEBUG_TOOLS */

	input_set_events_per_packet(input_device, 6 * CY_NUM_TCH_ID);

	dev_vdbg(ts->dev,
		"%s: Initialize irq\n", __func__);
#ifdef CY_USE_LEVEL_IRQ
	irq_flags = IRQF_TRIGGER_LOW | IRQF_ONESHOT;
#else
	irq_flags = IRQF_TRIGGER_FALLING | IRQF_ONESHOT;
#endif
	retval = request_threaded_irq(ts->irq, NULL, cyttsp4_irq,
		irq_flags, ts->input->name, ts);
	if (retval < 0) {
		dev_err(ts->dev,
			"%s: failed to init irq r=%d name=%s\n",
			__func__, retval, ts->input->name);
		ts->irq_enabled = false;
		goto error_init;
	} else {
		ts->irq_enabled = true;
	}

	retval = input_register_device(input_device);
	if (retval < 0) {
		dev_err(ts->dev,
			"%s: Error, failed to register input device r=%d\n",
			__func__, retval);
		goto error_init;
	}

#ifdef CONFIG_HAS_EARLYSUSPEND
	ts->early_suspend.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN + 1;
	ts->early_suspend.suspend = cyttsp4_early_suspend;
	ts->early_suspend.resume = cyttsp4_late_resume;
	register_early_suspend(&ts->early_suspend);
#endif

	INIT_WORK(&ts->cyttsp4_resume_startup_work, cyttsp4_ts_work_func);

	goto no_error;

error_init:
	mutex_destroy(&ts->data_lock);
	if (ts->cyttsp4_wq) {
		destroy_workqueue(ts->cyttsp4_wq);
		ts->cyttsp4_wq = NULL;
	}
error_alloc_failed:
#ifdef CONFIG_TOUCHSCREEN_DEBUG
	if (ts->fwname != NULL) {
		kfree(ts->fwname);
		ts->fwname = NULL;
	}
	if (ts->pr_buf != NULL) {
		kfree(ts->pr_buf);
		ts->pr_buf = NULL;
	}
#endif /* --CONFIG_TOUCHSCREEN_DEBUG */
	if (ts != NULL) {
		kfree(ts);
		ts = NULL;
	}
error_alloc_data:
	dev_err(ts->dev,
			"%s: Failed Initialization\n", __func__);
no_error:
	return ts;
}
EXPORT_SYMBOL_GPL(cyttsp4_core_init);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Cypress TrueTouch(R) Standard touchscreen driver core");
MODULE_AUTHOR("Cypress");

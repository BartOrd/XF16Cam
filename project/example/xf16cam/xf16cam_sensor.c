#include <stdio.h>

#include "compiler.h"
#include "driver/chip/hal_gpio.h"
#include "driver/chip/hal_i2c.h"
#include "driver/component/csi_camera/camera.h"
#include "kernel/os/os.h"

#include "xf16cam_sensor.h"
#include "xf16cam_sensor_tables.h"

#define XF16CAM_SENSOR_SETTLE_MS (100)
#define XF16CAM_SENSOR_WRITE_ATTEMPTS (4)
#define XF16CAM_SENSOR_ID_COUNT (2)

typedef struct {
	uint8_t reg;
	uint8_t value;
} XF16CamSensorId;

typedef struct {
	uint8_t input_seq;
	uint8_t vref_pol;
	uint8_t href_pol;
	uint8_t clk_pol;
	uint8_t sync_type;
} XF16CamCsiProfile;

/* Match camera.c unless a sensor descriptor explicitly says otherwise. */
#define XF16CAM_CSI_PROFILE(_seq, _vref, _href, _pclk, _sync) { \
	.input_seq = (_seq),                                      \
	.vref_pol = (_vref),                                      \
	.href_pol = (_href),                                      \
	.clk_pol = (_pclk),                                       \
	.sync_type = (_sync),                                     \
}

#define XF16CAM_CSI_DEFAULT \
	XF16CAM_CSI_PROFILE(CSI_IN_SEQ_YUYV, CSI_POL_POSITIVE, \
	                     CSI_POL_POSITIVE, CSI_POL_NEGATIVE, \
	                     CSI_SYNC_SEPARARE)

typedef struct {
	const char *name;
	const uint8_t *probe_table;
	const uint8_t *table;
	const uint8_t *post_table;
	uint16_t probe_table_size;
	uint16_t table_size;
	uint16_t post_table_size;
	uint16_t init_settle_ms;
	uint16_t input_width;
	uint16_t input_height;
	uint16_t output_width;
	uint16_t output_height;
	uint8_t address;
	uint8_t bank_register;
	uint8_t bank_value;
	XF16CamSensorId id[XF16CAM_SENSOR_ID_COUNT];
	uint8_t id_count;
	XF16CamCsiProfile csi;
	uint8_t vga_selectable;
	uint8_t power_cycle;
	uint8_t delay_register[2];
	uint8_t delay_value[2];
	uint8_t delay_ms[2];
} XF16CamSensor;

/* Every sensor shares one compact probe and register-table backend. */
__xip_rodata static const XF16CamSensor g_sensors[] = {
	{
		.name = "OV7690",
		.address = 0x21,
		.bank_register = 0xff,
		.bank_value = 0xff,
		.id = { { 0x0a, 0x76 }, { 0x0b, 0x91 } },
		.id_count = 2,
		.table = xf16cam_ov7690_table,
		.table_size = XF16CAM_OV7690_TABLE_SIZE,
		.init_settle_ms = 500,
		.input_width = 640,
		.input_height = 480,
		.output_width = 320,
		.output_height = 240,
		.vga_selectable = 1,
		.csi = XF16CAM_CSI_DEFAULT,
	},
	{
		.name = "GC0328",
		.table = (const uint8_t *)gc0328c_init_reg_tbl,
		.post_table = (const uint8_t *)gc0328c_post_init_reg_tbl,
		.table_size = XF16CAM_GC0328_TABLE_SIZE,
		.post_table_size = XF16CAM_GC0328_POST_TABLE_SIZE,
		.init_settle_ms = 2000,
		.address = 0x21,
		.bank_register = 0xfe,
		.bank_value = 0x00,
		.id = { { 0xf0, 0x9d } },
		.id_count = 1,
		.power_cycle = 1,
		.delay_register = { 0xfe, 0xfc },
		.delay_value = { 0x80, 0x16 },
		.delay_ms = { 10, 1 },
		.input_width = 320,
		.input_height = 240,
		.output_width = 320,
		.output_height = 240,
		.csi = XF16CAM_CSI_DEFAULT,
	},
	{
		.name = "GC0308",
		.address = 0x21,
		.bank_register = 0xfe,
		.bank_value = 0x00,
		.id = { { 0x00, 0x9b } },
		.id_count = 1,
		.table = xf16cam_gc0308_table,
		.table_size = XF16CAM_GC0308_TABLE_SIZE,
		.input_width = 640,
		.input_height = 480,
		.output_width = 320,
		.output_height = 240,
		.vga_selectable = 1,
		.csi = XF16CAM_CSI_DEFAULT,
	},
	{
		.name = "GC0309",
		.address = 0x21,
		.bank_register = 0xfe,
		.bank_value = 0x00,
		.id = { { 0x00, 0xa0 } },
		.id_count = 1,
		.table = xf16cam_gc0309_table,
		.table_size = XF16CAM_GC0309_TABLE_SIZE,
		.init_settle_ms = 1000,
		.input_width = 640,
		.input_height = 480,
		.output_width = 320,
		.output_height = 240,
		.vga_selectable = 1,
		.power_cycle = 1,
		.csi = XF16CAM_CSI_DEFAULT,
	},
	{
		.name = "GC0311",
		.address = 0x33,
		.bank_register = 0xfe,
		.bank_value = 0x00,
		.id = { { 0xf0, 0xbb } },
		.id_count = 1,
		.table = xf16cam_gc0311_table,
		.table_size = XF16CAM_GC0311_TABLE_SIZE,
		.post_table = xf16cam_gc0311_vga_table,
		.post_table_size = XF16CAM_GC0311_VGA_TABLE_SIZE,
		.init_settle_ms = 1000,
		.input_width = 640,
		.input_height = 480,
		.output_width = 320,
		.output_height = 240,
		.vga_selectable = 1,
		.power_cycle = 1,
		.csi = XF16CAM_CSI_DEFAULT,
	},
	{
		.name = "GC0329",
		.address = 0x31,
		.probe_table = xf16cam_gc0329_table,
		.probe_table_size = XF16CAM_GC0329_PROBE_TABLE_SIZE,
		.bank_register = 0xfe,
		.bank_value = 0x00,
		.id = { { 0x00, 0xc0 } },
		.id_count = 1,
		.table = xf16cam_gc0329_table,
		.table_size = XF16CAM_GC0329_TABLE_SIZE,
		.post_table = xf16cam_gc0329_vga_table,
		.post_table_size = XF16CAM_GC0329_VGA_TABLE_SIZE,
		.init_settle_ms = 500,
		.input_width = 640,
		.input_height = 480,
		.output_width = 320,
		.output_height = 240,
		.vga_selectable = 1,
		.csi = XF16CAM_CSI_DEFAULT,
	},
	{
		.name = "HI704",
		.address = 0x30,
		.bank_register = 0x03,
		.bank_value = 0x00,
		.id = { { 0x04, 0x96 } },
		.id_count = 1,
		.csi = XF16CAM_CSI_PROFILE(CSI_IN_SEQ_YUYV, CSI_POL_POSITIVE,
		                             CSI_POL_POSITIVE, CSI_POL_POSITIVE,
		                             CSI_SYNC_SEPARARE),
		.table = xf16cam_hi704_table,
		.table_size = XF16CAM_HI704_TABLE_SIZE,
		.input_width = 640,
		.input_height = 480,
		.output_width = 320,
		.output_height = 240,
		.vga_selectable = 1,
	},
	{
		.name = "GC0310",
		.address = 0x21,
		.bank_register = 0xfe,
		.bank_value = 0x00,
		.id = { { 0xf0, 0xa3 }, { 0xf1, 0x10 } },
		.id_count = 2,
		.table = xf16cam_gc0310_gc0312_table,
		.table_size = XF16CAM_GC0310_TABLE_SIZE,
		.init_settle_ms = 500,
		.input_width = 640,
		.input_height = 480,
		.output_width = 320,
		.output_height = 240,
		.vga_selectable = 1,
		.csi = XF16CAM_CSI_DEFAULT,
	},
	{
		.name = "GC0312",
		.address = 0x21,
		.bank_register = 0xfe,
		.bank_value = 0x00,
		.id = { { 0xf0, 0xb3 }, { 0xf1, 0x10 } },
		.id_count = 2,
		.table = xf16cam_gc0310_gc0312_table,
		.table_size = XF16CAM_GC0312_TABLE_SIZE,
		.input_width = 640,
		.input_height = 480,
		.output_width = 320,
		.output_height = 240,
		.vga_selectable = 1,
		.csi = XF16CAM_CSI_DEFAULT,
	},
	{
		.name = "SP0A19",
		.address = 0x21,
		.bank_register = 0xfd,
		.bank_value = 0x00,
		.id = { { 0x02, 0xa6 } },
		.id_count = 1,
		.table = xf16cam_sp0a19_table,
		.table_size = XF16CAM_SP0A19_TABLE_SIZE,
		.init_settle_ms = 1000,
		.input_width = 640,
		.input_height = 480,
		.output_width = 320,
		.output_height = 240,
		.vga_selectable = 1,
		.power_cycle = 1,
		.csi = XF16CAM_CSI_DEFAULT,
	},
	{
		.name = "SP0A20",
		.address = 0x21,
		.bank_register = 0xfd,
		.bank_value = 0x00,
		.id = { { 0x02, 0x2b } },
		.id_count = 1,
		.table = xf16cam_sp0a20_table,
		.table_size = XF16CAM_SP0A20_TABLE_SIZE,
		.input_width = 640,
		.input_height = 480,
		.output_width = 320,
		.output_height = 240,
		.vga_selectable = 1,
		.csi = XF16CAM_CSI_DEFAULT,
	},
	{
		.name = "SP0A39",
		.address = 0x21,
		.bank_register = 0xfd,
		.bank_value = 0x00,
		.id = { { 0x00, 0x0a }, { 0x01, 0x39 } },
		.id_count = 2,
		.table = xf16cam_sp0a39_table,
		.table_size = XF16CAM_SP0A39_TABLE_SIZE,
		.init_settle_ms = 500,
		.input_width = 640,
		.input_height = 480,
		.output_width = 320,
		.output_height = 240,
		.vga_selectable = 1,
		.csi = XF16CAM_CSI_DEFAULT,
	},
	{
		.name = "SP0828",
		.address = 0x18,
		.bank_register = 0xfd,
		.bank_value = 0x00,
		.id = { { 0x02, 0x0c } },
		.id_count = 1,
		.table = xf16cam_sp0828_table,
		.table_size = XF16CAM_SP0828_TABLE_SIZE,
		.input_width = 240,
		.input_height = 320,
		.output_width = 240,
		.output_height = 320,
		.csi = XF16CAM_CSI_DEFAULT,
	},
};

static const XF16CamSensor *g_selected;
static int g_sensor_night_mode;
static uint16_t g_output_width;
static uint16_t g_output_height;

__xip_text
void xf16cam_sensor_prepare_capture(void)
{
	const uint32_t mask = CSI_INPUT_SEQ_MASK | CSI_VREF_POL_MASK |
	                      CSI_HREF_POL_MASK | CSI_CLK_POL_MASK |
	                      CSI_SYNC_TYPE_MASK;
	uint32_t desired;

	if (!g_selected)
		return;
	desired = ((uint32_t)g_selected->csi.input_seq << CSI_INPUT_SEQ_SHIFT) |
	          ((uint32_t)g_selected->csi.vref_pol << CSI_VREF_POL_SHIFT) |
	          ((uint32_t)g_selected->csi.href_pol << CSI_HREF_POL_SHIFT) |
	          ((uint32_t)g_selected->csi.clk_pol << CSI_CLK_POL_SHIFT) |
	          ((uint32_t)g_selected->csi.sync_type << CSI_SYNC_TYPE_SHIFT);
	if ((CSI->CSI_CFG_REG & mask) != desired)
		HAL_MODIFY_REG(CSI->CSI_CFG_REG, mask, desired);
}

__xip_text
static void xf16cam_sensor_control(const SENSOR_ConfigParam *cfg, GPIO_PinState state)
{
	GPIO_InitParam param;

	param.driving = GPIO_DRIVING_LEVEL_1;
	param.mode = GPIOx_Pn_F1_OUTPUT;
	param.pull = GPIO_PULL_NONE;
	HAL_GPIO_Init(cfg->pwcfg.Pwdn_Port, cfg->pwcfg.Pwdn_Pin, &param);
	HAL_GPIO_WritePin(cfg->pwcfg.Pwdn_Port, cfg->pwcfg.Pwdn_Pin, state);
	OS_MSleep(XF16CAM_SENSOR_SETTLE_MS);
}

__xip_text
static HAL_Status xf16cam_sccb_init(I2C_ID bus)
{
	I2C_InitParam param;
	HAL_Status status;

	param.addrMode = I2C_ADDR_MODE_7BIT;
	param.clockFreq = 100000;
	status = HAL_I2C_Init(bus, &param);
	if (status != HAL_OK)
		printf("xf16cam camera: SCCB init failed (%ld)\n", (long)status);
	return status;
}

__xip_text
static void xf16cam_sensor_power_cycle(const SENSOR_ConfigParam *cfg)
{
	HAL_GPIO_WritePin(cfg->pwcfg.Pwdn_Port, cfg->pwcfg.Pwdn_Pin, GPIO_PIN_LOW);
	OS_MSleep(10);
	HAL_GPIO_WritePin(cfg->pwcfg.Pwdn_Port, cfg->pwcfg.Pwdn_Pin, GPIO_PIN_HIGH);
	OS_MSleep(10);
}

static HAL_Status xf16cam_sensor_write_table(I2C_ID bus,
					     const XF16CamSensor *sensor,
					     const uint8_t *table,
					     uint16_t size,
					     int report_error);

__xip_text
static int xf16cam_sensor_probe(I2C_ID bus, const XF16CamSensor *sensor,
				uint8_t *chip_id)
{
	uint8_t value = sensor->bank_value;
	unsigned int index;

	if (sensor->probe_table && sensor->probe_table_size &&
	    xf16cam_sensor_write_table(bus, sensor, sensor->probe_table,
	                               sensor->probe_table_size, 0) != HAL_OK)
		return 0;
	/* Factory descriptors use FF/FF for sensors without a bank selector. */
	if ((sensor->bank_register != 0xff || sensor->bank_value != 0xff) &&
	    HAL_I2C_SCCB_Master_Transmit_IT(bus, sensor->address,
	                                    sensor->bank_register, &value) != 1)
		return 0;
	if (sensor->id_count == 0 || sensor->id_count > XF16CAM_SENSOR_ID_COUNT)
		return 0;
	for (index = 0; index < sensor->id_count; ++index) {
		const XF16CamSensorId *id = &sensor->id[index];

		value = 0;
		if (HAL_I2C_SCCB_Master_Receive_IT(bus, sensor->address,
		                                 id->reg, &value) != 1)
			return 0;
		if (index == 0)
			*chip_id = value;
		if (value != id->value)
			return 0;
	}
	return 1;
}

__xip_text
static HAL_Status xf16cam_sensor_write_table(I2C_ID bus,
					     const XF16CamSensor *sensor,
					     const uint8_t *table,
					     uint16_t size,
					     int report_error)
{
	uint16_t offset;

	for (offset = 0; offset + 1 < size; offset += 2) {
		uint8_t reg = table[offset];
		uint8_t value = table[offset + 1];
		int attempt;
		int delay;

		if (reg == 0xff && value == 0xff)
			return HAL_OK;
		if (reg == 0xff && (value == 0xfe || value == 0xfd)) {
			OS_MSleep(value == 0xfe ? 100 : 1000);
			continue;
		}
		for (attempt = 0; attempt < XF16CAM_SENSOR_WRITE_ATTEMPTS; ++attempt) {
			uint8_t data = value;
			if (HAL_I2C_SCCB_Master_Transmit_IT(bus, sensor->address,
			                                  reg, &data) == 1)
				break;
			OS_MSleep(2);
		}
		if (attempt == XF16CAM_SENSOR_WRITE_ATTEMPTS) {
			if (report_error)
				printf("xf16cam camera: %s table write failed at %u\n",
				       sensor->name, (unsigned int)(offset / 2));
			return HAL_ERROR;
		}
		for (delay = 0; delay < 2; ++delay) {
			if (sensor->delay_ms[delay] &&
			    reg == sensor->delay_register[delay] &&
			    value == sensor->delay_value[delay])
				OS_MSleep(sensor->delay_ms[delay]);
		}
		if (offset == 0)
			OS_MSleep(1);
	}
	return HAL_OK;
}

__xip_text
static HAL_Status xf16cam_sensor_load_table(SENSOR_ConfigParam *cfg)
{
	I2C_ID bus = (I2C_ID)cfg->i2c_id;
	uint8_t chip_id;

	if (!g_selected || !g_selected->table || !g_selected->table_size)
		return HAL_ERROR;
	if (g_selected->power_cycle)
		xf16cam_sensor_power_cycle(cfg);
	if (xf16cam_sccb_init(bus) != HAL_OK)
		return HAL_ERROR;
	if (g_selected->power_cycle &&
	    !xf16cam_sensor_probe(bus, g_selected, &chip_id)) {
		printf("xf16cam camera: %s did not return after power cycle\n",
		       g_selected->name);
		HAL_I2C_DeInit(bus);
		return HAL_ERROR;
	}
	if (xf16cam_sensor_write_table(bus, g_selected, g_selected->table,
	                                g_selected->table_size, 1) != HAL_OK ||
	    (g_selected->post_table &&
	     xf16cam_sensor_write_table(bus, g_selected, g_selected->post_table,
	                                 g_selected->post_table_size, 1) != HAL_OK)) {
		HAL_I2C_DeInit(bus);
		return HAL_ERROR;
	}
	if (g_selected->init_settle_ms)
		OS_MSleep(g_selected->init_settle_ms);

	printf("xf16cam camera: %s init complete\n", g_selected->name);
	return HAL_OK;
}

__xip_text
HAL_Status xf16cam_sensor_init(SENSOR_ConfigParam *cfg)
{
	static const GPIO_PinState control_states[] = { GPIO_PIN_HIGH, GPIO_PIN_LOW };
	I2C_ID bus;
	uint8_t chip_id = 0;
	unsigned int phase;
	unsigned int index;

	if (!cfg)
		return HAL_ERROR;
	bus = (I2C_ID)cfg->i2c_id;
	g_selected = NULL;
	g_output_width = 0;
	g_output_height = 0;

	for (phase = 0; phase < sizeof(control_states) / sizeof(control_states[0]); ++phase) {
		xf16cam_sensor_control(cfg, control_states[phase]);
		if (xf16cam_sccb_init(bus) != HAL_OK)
			return HAL_ERROR;
		for (index = 0; index < sizeof(g_sensors) / sizeof(g_sensors[0]); ++index) {
			if (xf16cam_sensor_probe(bus, &g_sensors[index], &chip_id)) {
				HAL_Status status;

				g_selected = &g_sensors[index];
				g_output_width = g_selected->output_width;
				g_output_height = g_selected->output_height;
				HAL_I2C_DeInit(bus);
				printf("xf16cam camera: %s detected (id=0x%02x)\n",
				       g_selected->name, chip_id);
				status = xf16cam_sensor_load_table(cfg);
				if (status != HAL_OK)
					g_selected = NULL;
				return status;
			}
		}
		HAL_I2C_DeInit(bus);
	}

	printf("xf16cam camera: no supported sensor (last id=0x%02x)\n", chip_id);
	return HAL_ERROR;
}

__xip_text
void xf16cam_sensor_deinit(SENSOR_ConfigParam *cfg)
{
	if (cfg) {
		HAL_GPIO_WritePin(cfg->pwcfg.Pwdn_Port, cfg->pwcfg.Pwdn_Pin,
		                  GPIO_PIN_HIGH);
		OS_MSleep(3);
		HAL_GPIO_DeInit(cfg->pwcfg.Pwdn_Port, cfg->pwcfg.Pwdn_Pin);
		HAL_I2C_DeInit((I2C_ID)cfg->i2c_id);
	}
	/* Keep the last detected identity while the rail is idle so management can
	 * describe the sensor. The next init always probes again before capture. */
}

__xip_text
int xf16cam_sensor_configure_camera(uint16_t configured_width,
				    uint16_t configured_height)
{
	SENSOR_PixelSize input;
	int scale;

	if (!g_selected)
		return -1;
	g_output_width = g_selected->output_width;
	g_output_height = g_selected->output_height;
	if (g_selected->vga_selectable && configured_width == 640 &&
	    configured_height == 480) {
		g_output_width = g_selected->input_width;
		g_output_height = g_selected->input_height;
	}
	if (g_selected->input_width != configured_width ||
	    g_selected->input_height != configured_height ||
	    g_output_width != configured_width ||
	    g_output_height != configured_height) {
		input.width = g_selected->input_width;
		input.height = g_selected->input_height;
		if (HAL_CAMERA_IoCtl(CAMERA_SET_PIXEL_SIZE, (uint32_t)&input) != 0 ||
		    HAL_CAMERA_IoCtl(CAMERA_SET_JPEG_MODE, JPEG_MOD_ONLINE) != 0)
			return -1;

		scale = g_selected->input_width != g_output_width ||
		        g_selected->input_height != g_output_height;
		if (scale) {
			if (g_selected->input_width != g_output_width * 2 ||
			    g_selected->input_height != g_output_height * 2 ||
			    HAL_CAMERA_IoCtl(CAMERA_SET_JPEG_SCALE, 1) != 0)
				return -1;
		}
	}

	/* Sensor byte order and sync signals differ. Apply this after geometry
	 * ioctls, which reconfigure CSI with the SDK defaults. */
	xf16cam_sensor_prepare_capture();
	printf("xf16cam camera: %s output=%ux%u CSI seq=%u vref=%u href=%u pclk=%u sync=%u\n",
	       g_selected->name,
	       (unsigned int)g_output_width, (unsigned int)g_output_height,
	       (unsigned int)g_selected->csi.input_seq,
	       (unsigned int)g_selected->csi.vref_pol,
	       (unsigned int)g_selected->csi.href_pol,
	       (unsigned int)g_selected->csi.clk_pol,
	       (unsigned int)g_selected->csi.sync_type);

	if (g_sensor_night_mode)
		xf16cam_sensor_switch_cam_sensor_mode(1);
	return 0;
}

static const uint8_t g_sp0a39_night_mode[] = {
	0xFD, 0x01, 0x47, 0x20, 0xFD, 0x00,
};

static const uint8_t g_sp0a39_day_mode[] = {
	0xFD, 0x01, 0x47, 0x00, 0xFD, 0x00,
};

__xip_text
int xf16cam_sensor_switch_cam_sensor_mode(int night_mode)
{
	I2C_ID bus = I2C0_ID;
	const uint8_t *sequence;
	uint16_t length;
	uint16_t offset;

	g_sensor_night_mode = night_mode != 0;
	if (!g_selected || strcmp(g_selected->name, "SP0A39") != 0)
		return 0;

	sequence = g_sensor_night_mode ? g_sp0a39_night_mode : g_sp0a39_day_mode;
	length = g_sensor_night_mode ? sizeof(g_sp0a39_night_mode) : sizeof(g_sp0a39_day_mode);
	for (offset = 0; offset + 1 < length; offset += 2) {
		uint8_t reg = sequence[offset];
		uint8_t value = sequence[offset + 1];
		if (HAL_I2C_SCCB_Master_Transmit_IT(bus, g_selected->address, reg, &value) != 1)
			return -1;
	}
	return 0;
}

const char *xf16cam_sensor_name(void)
{
	return g_selected ? g_selected->name : "Not detected";
}

int xf16cam_sensor_available(void)
{
	return g_selected != NULL;
}

uint16_t xf16cam_sensor_width(void)
{
	return g_selected ? g_output_width : 0;
}

uint16_t xf16cam_sensor_height(void)
{
	return g_selected ? g_output_height : 0;
}

int xf16cam_sensor_supports_vga(void)
{
	return g_selected && g_selected->vga_selectable;
}

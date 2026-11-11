#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "xf16cam_motion.h"

#define XF16CAM_MOTION_GRID_W 32U
#define XF16CAM_MOTION_GRID_H 24U
#define XF16CAM_MOTION_GRID_SIZE (XF16CAM_MOTION_GRID_W * XF16CAM_MOTION_GRID_H)
#define XF16CAM_JPEG_COMPONENTS 3U
#define XF16CAM_HUFFMAN_TABLES 4U

typedef struct {
	int32_t minimum[17];
	int32_t maximum[17];
	uint16_t value_offset[17];
	uint8_t values[256];
	uint8_t valid;
} XF16CamHuffmanTable;

typedef struct {
	uint8_t id;
	uint8_t horizontal;
	uint8_t vertical;
	uint8_t dc_table;
	uint8_t ac_table;
	int32_t predictor;
} XF16CamJpegComponent;

typedef struct {
	const uint8_t *data;
	uint32_t length;
	uint32_t position;
	uint8_t value;
	uint8_t bits_left;
	int error;
} XF16CamBitReader;

static XF16CamHuffmanTable g_huffman[2][XF16CAM_HUFFMAN_TABLES];
static XF16CamJpegComponent g_components[XF16CAM_JPEG_COMPONENTS];
static int16_t g_previous[XF16CAM_MOTION_GRID_SIZE];
static int16_t g_current_dc[XF16CAM_MOTION_GRID_SIZE];
static uint8_t g_previous_valid[XF16CAM_MOTION_GRID_SIZE];
static uint8_t g_current_valid[XF16CAM_MOTION_GRID_SIZE];
static int g_previous_frame_valid;
static int g_motion_candidate;
static volatile int g_reset_requested;

static uint16_t xf16cam_motion_be16(const uint8_t *data)
{
	return (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
}

static int xf16cam_motion_huffman_build(XF16CamHuffmanTable *table,
	                                    const uint8_t *counts,
	                                    const uint8_t *values,
	                                    uint32_t value_count)
{
	uint32_t code = 0;
	uint32_t offset = 0;
	unsigned int length;

	if (value_count > sizeof(table->values))
		return -1;
	memset(table, 0, sizeof(*table));
	for (length = 1; length <= 16; ++length) {
		uint32_t count = counts[length - 1];
		if (count == 0) {
			table->minimum[length] = 1;
			table->maximum[length] = 0;
			table->value_offset[length] = (uint16_t)offset;
		} else {
			table->minimum[length] = (int32_t)code;
			table->maximum[length] = (int32_t)(code + count - 1U);
			table->value_offset[length] = (uint16_t)offset;
			offset += count;
			code += count;
			if (code > (1UL << length))
				return -1;
		}
		code <<= 1;
	}
	if (offset != value_count)
		return -1;
	memcpy(table->values, values, value_count);
	table->valid = 1;
	return 0;
}

static int xf16cam_motion_parse_dht(const uint8_t *data, uint32_t length)
{
	uint32_t position = 0;

	while (position < length) {
		uint8_t selector;
		uint32_t count = 0;
		unsigned int i;
		uint8_t counts[16];
		unsigned int table_class;
		unsigned int table_id;

		if (position + 17U > length)
			return -1;
		selector = data[position++];
		table_class = selector >> 4;
		table_id = selector & 0x0fU;
		if (table_class > 1U || table_id >= XF16CAM_HUFFMAN_TABLES)
			return -1;
		for (i = 0; i < 16; ++i) {
			counts[i] = data[position++];
			count += counts[i];
		}
		if (count > 256U || position + count > length)
			return -1;
		if (xf16cam_motion_huffman_build(&g_huffman[table_class][table_id],
		                                 counts, data + position, count) != 0)
			return -1;
		position += count;
	}
	return 0;
}

static int xf16cam_motion_read_byte(XF16CamBitReader *reader, uint8_t *value)
{
	uint8_t byte;

	if (reader->position >= reader->length)
		return -1;
	byte = reader->data[reader->position++];
	if (byte == 0xffU) {
		if (reader->position >= reader->length || reader->data[reader->position] != 0x00U)
			return -1;
		++reader->position;
	}
	*value = byte;
	return 0;
}

static int xf16cam_motion_read_bits(XF16CamBitReader *reader, unsigned int count,
	                                uint32_t *value)
{
	uint32_t result = 0;
	unsigned int i;

	if (count > 16U)
		return -1;
	for (i = 0; i < count; ++i) {
		if (reader->bits_left == 0U) {
			if (xf16cam_motion_read_byte(reader, &reader->value) != 0) {
				reader->error = 1;
				return -1;
			}
			reader->bits_left = 8U;
		}
		result = (result << 1) | ((reader->value >> (reader->bits_left - 1U)) & 1U);
		--reader->bits_left;
	}
	*value = result;
	return 0;
}

static int xf16cam_motion_decode_symbol(XF16CamBitReader *reader,
	                                    const XF16CamHuffmanTable *table,
	                                    uint8_t *symbol)
{
	int32_t code = 0;
	unsigned int length;

	if (!table->valid)
		return -1;
	for (length = 1; length <= 16; ++length) {
		uint32_t bit;
		if (xf16cam_motion_read_bits(reader, 1, &bit) != 0)
			return -1;
		code = (code << 1) | (int32_t)bit;
		if (code >= table->minimum[length] && code <= table->maximum[length]) {
			uint32_t index = table->value_offset[length] +
			                 (uint32_t)(code - table->minimum[length]);
			*symbol = table->values[index];
			return 0;
		}
	}
	return -1;
}

static int32_t xf16cam_motion_extend(uint32_t value, unsigned int size)
{
	uint32_t threshold;

	if (size == 0U)
		return 0;
	threshold = 1UL << (size - 1U);
	if (value < threshold)
		return (int32_t)value - (int32_t)((1UL << size) - 1UL);
	return (int32_t)value;
}

static int xf16cam_motion_skip_ac(XF16CamBitReader *reader,
	                              const XF16CamHuffmanTable *table)
{
	unsigned int coefficient = 1;

	while (coefficient <= 63U) {
		uint8_t run_size;
		unsigned int run;
		unsigned int size;
		uint32_t ignored;

		if (xf16cam_motion_decode_symbol(reader, table, &run_size) != 0)
			return -1;
		run = run_size >> 4;
		size = run_size & 0x0fU;
		if (size == 0U) {
			if (run == 0U)
				break;
			if (run != 15U)
				return -1;
			coefficient += 16U;
			continue;
		}
		coefficient += run;
		if (coefficient > 63U || xf16cam_motion_read_bits(reader, size, &ignored) != 0)
			return -1;
		++coefficient;
	}
	return 0;
}

static int xf16cam_motion_restart(XF16CamBitReader *reader, unsigned int expected)
{
	reader->bits_left = 0;
	while (reader->position < reader->length && reader->data[reader->position] == 0xffU)
		++reader->position;
	if (reader->position >= reader->length ||
	    reader->data[reader->position] != (uint8_t)(0xd0U + (expected & 7U)))
		return -1;
	++reader->position;
	return 0;
}

static int xf16cam_motion_parse_frame(const uint8_t *jpeg, uint32_t length,
	                                  XF16CamBitReader *reader,
	                                  uint32_t *width, uint32_t *height,
	                                  unsigned int *component_count,
	                                  unsigned int *max_horizontal,
	                                  unsigned int *max_vertical,
	                                  uint16_t *restart_interval)
{
	uint32_t position = 2;
	int found_frame = 0;
	int found_scan = 0;

	memset(g_huffman, 0, sizeof(g_huffman));
	memset(g_components, 0, sizeof(g_components));
	*restart_interval = 0;
	if (length < 4U || jpeg[0] != 0xffU || jpeg[1] != 0xd8U)
		return -1;

	while (position + 4U <= length) {
		uint8_t marker;
		uint16_t segment_length;
		const uint8_t *data;
		uint32_t data_length;

		while (position < length && jpeg[position] == 0xffU)
			++position;
		if (position >= length)
			break;
		marker = jpeg[position++];
		if (marker == 0xd9U)
			break;
		if (marker == 0x01U || (marker >= 0xd0U && marker <= 0xd7U))
			continue;
		if (position + 2U > length)
			return -1;
		segment_length = xf16cam_motion_be16(jpeg + position);
		if (segment_length < 2U || position + segment_length > length)
			return -1;
		data = jpeg + position + 2U;
		data_length = segment_length - 2U;

		if (marker == 0xc0U) {
			unsigned int i;
			if (data_length < 6U || data[0] != 8U)
				return -1;
			*height = xf16cam_motion_be16(data + 1);
			*width = xf16cam_motion_be16(data + 3);
			*component_count = data[5];
			if (*component_count == 0U || *component_count > XF16CAM_JPEG_COMPONENTS ||
			    data_length < 6U + 3U * *component_count || *width == 0U || *height == 0U)
				return -1;
			*max_horizontal = 0;
			*max_vertical = 0;
			for (i = 0; i < *component_count; ++i) {
				uint8_t sampling = data[7U + i * 3U];
				g_components[i].id = data[6U + i * 3U];
				g_components[i].horizontal = sampling >> 4;
				g_components[i].vertical = sampling & 0x0fU;
				if (g_components[i].horizontal == 0U || g_components[i].vertical == 0U)
					return -1;
				if (g_components[i].horizontal > *max_horizontal)
					*max_horizontal = g_components[i].horizontal;
				if (g_components[i].vertical > *max_vertical)
					*max_vertical = g_components[i].vertical;
			}
			found_frame = 1;
		} else if (marker == 0xc4U) {
			if (xf16cam_motion_parse_dht(data, data_length) != 0)
				return -1;
		} else if (marker == 0xddU) {
			if (data_length != 2U)
				return -1;
			*restart_interval = xf16cam_motion_be16(data);
		} else if (marker == 0xdaU) {
			unsigned int scan_components;
			unsigned int i;
			if (!found_frame || data_length < 4U)
				return -1;
			scan_components = data[0];
			if (scan_components != *component_count || data_length < 1U + 2U * scan_components + 3U)
				return -1;
			for (i = 0; i < scan_components; ++i) {
				unsigned int j;
				uint8_t id = data[1U + i * 2U];
				uint8_t tables = data[2U + i * 2U];
				for (j = 0; j < *component_count; ++j) {
					if (g_components[j].id == id)
						break;
				}
				if (j == *component_count)
					return -1;
				g_components[j].dc_table = tables >> 4;
				g_components[j].ac_table = tables & 0x0fU;
				if (g_components[j].dc_table >= XF16CAM_HUFFMAN_TABLES ||
				    g_components[j].ac_table >= XF16CAM_HUFFMAN_TABLES)
					return -1;
			}
			if (data[1U + 2U * scan_components] != 0U ||
			    data[2U + 2U * scan_components] != 63U ||
			    data[3U + 2U * scan_components] != 0U)
				return -1;
			reader->data = jpeg;
			reader->length = length;
			reader->position = position + segment_length;
			reader->bits_left = 0;
			reader->value = 0;
			reader->error = 0;
			found_scan = 1;
			break;
		}
		position += segment_length;
	}
	return found_frame && found_scan ? 0 : -1;
}

static int xf16cam_motion_decode_block(XF16CamBitReader *reader,
	                                   XF16CamJpegComponent *component,
	                                   int16_t *dc_value)
{
	uint8_t category;
	uint32_t bits = 0;
	int32_t difference;

	if (xf16cam_motion_decode_symbol(reader,
	        &g_huffman[0][component->dc_table], &category) != 0 || category > 11U)
		return -1;
	if (xf16cam_motion_read_bits(reader, category, &bits) != 0)
		return -1;
	difference = xf16cam_motion_extend(bits, category);
	component->predictor += difference;
	if (component->predictor > 32767)
		component->predictor = 32767;
	else if (component->predictor < -32768)
		component->predictor = -32768;
	*dc_value = (int16_t)component->predictor;
	return xf16cam_motion_skip_ac(reader, &g_huffman[1][component->ac_table]);
}

void xf16cam_motion_reset(void)
{
	g_reset_requested = 1;
}

int xf16cam_motion_process_jpeg(const uint8_t *jpeg, uint32_t length,
	                            uint8_t sensitivity)
{
	XF16CamBitReader reader;
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t blocks_per_row;
	uint32_t blocks_per_column;
	uint32_t mcu_columns;
	uint32_t mcu_rows;
	uint32_t mcu_number = 0;
	uint32_t total_mcus;
	uint32_t valid_samples = 0;
	uint32_t changed_samples = 0;
	uint32_t changed_threshold;
	uint32_t required_changes;
	unsigned int effective_sensitivity = sensitivity > 100U ? 100U : sensitivity;
	unsigned int component_count = 0;
	unsigned int max_horizontal = 0;
	unsigned int max_vertical = 0;
	unsigned int y_component = 0;
	uint16_t restart_interval = 0;
	unsigned int restart_number = 0;
	unsigned int i;
	int result = 0;

	if (g_reset_requested) {
		memset(g_previous_valid, 0, sizeof(g_previous_valid));
		g_previous_frame_valid = 0;
		g_motion_candidate = 0;
		g_reset_requested = 0;
	}
	if (jpeg == NULL || xf16cam_motion_parse_frame(jpeg, length, &reader,
	        &width, &height, &component_count, &max_horizontal,
	        &max_vertical, &restart_interval) != 0)
		return 0;
	for (i = 0; i < component_count; ++i) {
		if (g_components[i].dc_table >= XF16CAM_HUFFMAN_TABLES ||
		    !g_huffman[0][g_components[i].dc_table].valid ||
		    !g_huffman[1][g_components[i].ac_table].valid)
			return 0;
	}

	blocks_per_row = (width + 7U) / 8U;
	blocks_per_column = (height + 7U) / 8U;
	mcu_columns = (width + 8U * max_horizontal - 1U) / (8U * max_horizontal);
	mcu_rows = (height + 8U * max_vertical - 1U) / (8U * max_vertical);
	total_mcus = mcu_columns * mcu_rows;
	if (total_mcus == 0U || total_mcus > 4096U)
		return 0;
	changed_threshold = 3U + (100U - effective_sensitivity) / 12U;
	memset(g_current_valid, 0, sizeof(g_current_valid));
	for (i = 0; i < component_count; ++i)
		g_components[i].predictor = 0;

	for (mcu_number = 0; mcu_number < total_mcus; ++mcu_number) {
		uint32_t mcu_x = mcu_number % mcu_columns;
		uint32_t mcu_y = mcu_number / mcu_columns;
		unsigned int component_index;

		for (component_index = 0; component_index < component_count; ++component_index) {
			unsigned int by;
			unsigned int bx;
			XF16CamJpegComponent *component;
			component = &g_components[component_index];
			for (by = 0; by < component->vertical; ++by) {
				for (bx = 0; bx < component->horizontal; ++bx) {
					int16_t dc;
					if (xf16cam_motion_decode_block(&reader, component, &dc) != 0)
						return 0;
					if (component_index == y_component) {
						uint32_t block_x = mcu_x * component->horizontal + bx;
						uint32_t block_y = mcu_y * component->vertical + by;
						if (block_x < blocks_per_row && block_y < blocks_per_column) {
							uint32_t grid_x = block_x * XF16CAM_MOTION_GRID_W / blocks_per_row;
							uint32_t grid_y = block_y * XF16CAM_MOTION_GRID_H / blocks_per_column;
							uint32_t index = grid_y * XF16CAM_MOTION_GRID_W + grid_x;
							if (!g_current_valid[index]) {
								g_current_valid[index] = 1;
								g_current_dc[index] = dc;
							}
						}
					}
				}
			}
		}
		if (restart_interval != 0U && (mcu_number + 1U) < total_mcus &&
		    ((mcu_number + 1U) % restart_interval) == 0U) {
			if (xf16cam_motion_restart(&reader, restart_number++) != 0)
				return 0;
			for (i = 0; i < component_count; ++i)
				g_components[i].predictor = 0;
		}
	}

	for (i = 0; i < XF16CAM_MOTION_GRID_SIZE; ++i) {
		if (g_current_valid[i]) {
			++valid_samples;
			if (g_previous_frame_valid && g_previous_valid[i]) {
				int difference = (int)g_current_dc[i] - (int)g_previous[i];
				if (difference < 0)
					difference = -difference;
				if ((unsigned int)difference >= changed_threshold)
					++changed_samples;
			}
			g_previous[i] = g_current_dc[i];
		}
		g_previous_valid[i] = g_current_valid[i];
	}
	required_changes = (valid_samples * (8U + (100U - effective_sensitivity) / 12U) + 999U) / 1000U;
	if (required_changes < 6U)
		required_changes = 6U;
	g_previous_frame_valid = valid_samples >= 64U;
	if (!g_previous_frame_valid)
		return 0;

	if (changed_samples >= required_changes) {
		if (g_motion_candidate)
			result = 1;
		g_motion_candidate = 1;
	} else {
		g_motion_candidate = 0;
	}
	return result;
}

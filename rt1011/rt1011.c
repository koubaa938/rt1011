#include "rt1011.h"
#include "registers.h"

#define bool int

static ULONG Rt1011DebugLevel = 100;
static ULONG Rt1011DebugCatagories = DBG_INIT || DBG_PNP || DBG_IOCTL;

#define BITS_PER_LONG 32

unsigned int __sw_hweight32(unsigned int w)
{
	w -= (w >> 1) & 0x55555555;
	w = (w & 0x33333333) + ((w >> 2) & 0x33333333);
	w = (w + (w >> 4)) & 0x0f0f0f0f;
	return (w * 0x01010101) >> 24;
}
#define hweight_long __sw_hweight32

static unsigned long __fls(unsigned long word)
{
	int num = BITS_PER_LONG - 1;
	if (!(word & (~0ul << (BITS_PER_LONG - 16)))) {
		num -= 16;
		word <<= 16;
	}
	if (!(word & (~0ul << (BITS_PER_LONG - 8)))) {
		num -= 8;
		word <<= 8;
	}
	if (!(word & (~0ul << (BITS_PER_LONG - 4)))) {
		num -= 4;
		word <<= 4;
	}
	if (!(word & (~0ul << (BITS_PER_LONG - 2)))) {
		num -= 2;
		word <<= 2;
	}
	if (!(word & (~0ul << (BITS_PER_LONG - 1))))
		num -= 1;
	return num;
}

unsigned long __ffs(unsigned long word)
{
	int num = 0;

	if ((word & 0xffff) == 0) {
		num += 16;
		word >>= 16;
	}
	if ((word & 0xff) == 0) {
		num += 8;
		word >>= 8;
	}
	if ((word & 0xf) == 0) {
		num += 4;
		word >>= 4;
	}
	if ((word & 0x3) == 0) {
		num += 2;
		word >>= 2;
	}
	if ((word & 0x1) == 0)
		num += 1;
	return num;
}

NTSTATUS
DriverEntry(
	__in PDRIVER_OBJECT  DriverObject,
	__in PUNICODE_STRING RegistryPath
)
{
	NTSTATUS               status = STATUS_SUCCESS;
	WDF_DRIVER_CONFIG      config;
	WDF_OBJECT_ATTRIBUTES  attributes;

	Rt1011Print(DEBUG_LEVEL_INFO, DBG_INIT,
		"Driver Entry\n");

	WDF_DRIVER_CONFIG_INIT(&config, Rt1011EvtDeviceAdd);

	WDF_OBJECT_ATTRIBUTES_INIT(&attributes);

	//
	// Create a framework driver object to represent our driver.
	//

	status = WdfDriverCreate(DriverObject,
		RegistryPath,
		&attributes,
		&config,
		WDF_NO_HANDLE
	);

	if (!NT_SUCCESS(status))
	{
		Rt1011Print(DEBUG_LEVEL_ERROR, DBG_INIT,
			"WdfDriverCreate failed with status 0x%x\n", status);
	}

	return status;
}

static NTSTATUS rt1011_reg_write(PRT1011_CONTEXT pDevice, uint16_t reg, uint16_t data)
{
	uint16_t rawdata[2];
	rawdata[0] = RtlUshortByteSwap(reg);
	rawdata[1] = RtlUshortByteSwap(data);
	return SpbWriteDataSynchronously(&pDevice->I2CContext, rawdata, sizeof(rawdata));
}

struct reg {
	UINT16 reg;
	UINT16 val;
};

static NTSTATUS rt1011_reg_read(PRT1011_CONTEXT pDevice, uint16_t reg, uint16_t* data)
{
	uint16_t reg_swap = RtlUshortByteSwap(reg);
	uint16_t data_swap = 0;
	NTSTATUS ret = SpbXferDataSynchronously(&pDevice->I2CContext, &reg_swap, sizeof(uint16_t), &data_swap, sizeof(uint16_t));
	*data = RtlUshortByteSwap(data_swap);
	return ret;
}

static NTSTATUS rt1011_reg_update(
	_In_ PRT1011_CONTEXT pDevice,
	uint16_t reg,
	uint16_t mask,
	uint16_t val
) {
	uint16_t tmp = 0, orig = 0;

	NTSTATUS status = rt1011_reg_read(pDevice, reg, &orig);
	if (!NT_SUCCESS(status)) {
		return status;
	}

	tmp = orig & ~mask;
	tmp |= val & mask;

	if (tmp != orig) {
		status = rt1011_reg_write(pDevice, reg, tmp);
	}
	return status;
}

static NTSTATUS rt1011_reg_burstWrite(PRT1011_CONTEXT pDevice, struct reg* regs, int regCount) {
	NTSTATUS status = STATUS_NO_MEMORY;
	for (int i = 0; i < regCount; i++) {
		struct reg* regToSet = &regs[i];
		status = rt1011_reg_write(pDevice, regToSet->reg, regToSet->val);
		if (!NT_SUCCESS(status)) {
			return status;
		}
	}
	return status;
}

NTSTATUS
GetDeviceUID(
	_In_ WDFDEVICE FxDevice,
	_In_ PINT32 PUID
)
{
	NTSTATUS status = STATUS_ACPI_NOT_INITIALIZED;
	ACPI_EVAL_INPUT_BUFFER_EX inputBuffer;
	RtlZeroMemory(&inputBuffer, sizeof(inputBuffer));

	inputBuffer.Signature = ACPI_EVAL_INPUT_BUFFER_SIGNATURE_EX;
	status = RtlStringCchPrintfA(
		inputBuffer.MethodName,
		sizeof(inputBuffer.MethodName),
		"_UID"
	);
	if (!NT_SUCCESS(status)) {
		return status;
	}

	WDFMEMORY outputMemory;
	PACPI_EVAL_OUTPUT_BUFFER outputBuffer;
	size_t outputArgumentBufferSize = 32;
	size_t outputBufferSize = FIELD_OFFSET(ACPI_EVAL_OUTPUT_BUFFER, Argument) + outputArgumentBufferSize;

	WDF_OBJECT_ATTRIBUTES attributes;
	WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
	attributes.ParentObject = FxDevice;

	status = WdfMemoryCreate(&attributes,
		NonPagedPoolNx,
		0,
		outputBufferSize,
		&outputMemory,
		(PVOID*)&outputBuffer);
	if (!NT_SUCCESS(status)) {
		return status;
	}

	RtlZeroMemory(outputBuffer, outputBufferSize);

	WDF_MEMORY_DESCRIPTOR inputMemDesc;
	WDF_MEMORY_DESCRIPTOR outputMemDesc;
	WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&inputMemDesc, &inputBuffer, (ULONG)sizeof(inputBuffer));
	WDF_MEMORY_DESCRIPTOR_INIT_HANDLE(&outputMemDesc, outputMemory, NULL);

	status = WdfIoTargetSendInternalIoctlSynchronously(
		WdfDeviceGetIoTarget(FxDevice),
		NULL,
		IOCTL_ACPI_EVAL_METHOD_EX,
		&inputMemDesc,
		&outputMemDesc,
		NULL,
		NULL
	);
	if (!NT_SUCCESS(status)) {
		goto Exit;
	}

	if (outputBuffer->Signature != ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE) {
		goto Exit;
	}

	if (outputBuffer->Count < 1) {
		goto Exit;
	}

	uint32_t uid;
	if (outputBuffer->Argument[0].DataLength >= 4) {
		uid = *(uint32_t*)outputBuffer->Argument->Data;
	}
	else if (outputBuffer->Argument[0].DataLength >= 2) {
		uid = *(uint16_t*)outputBuffer->Argument->Data;
	}
	else {
		uid = *(uint8_t*)outputBuffer->Argument->Data;
	}
	if (PUID) {
		*PUID = uid;
	}
	else {
		status = STATUS_ACPI_INVALID_ARGUMENT;
	}
Exit:
	if (outputMemory != WDF_NO_HANDLE) {
		WdfObjectDelete(outputMemory);
	}
	return status;
}

static NTSTATUS GetIntegerProperty(
	_In_ WDFDEVICE FxDevice,
	char* propertyStr,
	UINT16* property
) {
	WDFMEMORY outputMemory = WDF_NO_HANDLE;

	NTSTATUS status = STATUS_ACPI_NOT_INITIALIZED;

	size_t inputBufferLen = sizeof(ACPI_GET_DEVICE_SPECIFIC_DATA) + strlen(propertyStr) + 1;
	ACPI_GET_DEVICE_SPECIFIC_DATA* inputBuffer = ExAllocatePoolWithTag(NonPagedPool, inputBufferLen, RT1011_POOL_TAG);
	if (!inputBuffer) {
		goto Exit;
	}
	RtlZeroMemory(inputBuffer, inputBufferLen);

	inputBuffer->Signature = IOCTL_ACPI_GET_DEVICE_SPECIFIC_DATA_SIGNATURE;

	unsigned char uuidend[] = { 0x8a, 0x91, 0xbc, 0x9b, 0xbf, 0x4a, 0xa3, 0x01 };

	inputBuffer->Section.Data1 = 0xdaffd814;
	inputBuffer->Section.Data2 = 0x6eba;
	inputBuffer->Section.Data3 = 0x4d8c;
	memcpy(inputBuffer->Section.Data4, uuidend, sizeof(uuidend)); //Avoid Windows defender false positive

	strcpy(inputBuffer->PropertyName, propertyStr);
	inputBuffer->PropertyNameLength = strlen(propertyStr) + 1;

	PACPI_EVAL_OUTPUT_BUFFER outputBuffer;
	size_t outputArgumentBufferSize = 8;
	size_t outputBufferSize = FIELD_OFFSET(ACPI_EVAL_OUTPUT_BUFFER, Argument) + sizeof(ACPI_METHOD_ARGUMENT_V1) + outputArgumentBufferSize;

	WDF_OBJECT_ATTRIBUTES attributes;
	WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
	attributes.ParentObject = FxDevice;
	status = WdfMemoryCreate(&attributes,
		NonPagedPoolNx,
		0,
		outputBufferSize,
		&outputMemory,
		&outputBuffer);
	if (!NT_SUCCESS(status)) {
		goto Exit;
	}

	WDF_MEMORY_DESCRIPTOR inputMemDesc;
	WDF_MEMORY_DESCRIPTOR outputMemDesc;
	WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&inputMemDesc, inputBuffer, (ULONG)inputBufferLen);
	WDF_MEMORY_DESCRIPTOR_INIT_HANDLE(&outputMemDesc, outputMemory, NULL);

	status = WdfIoTargetSendInternalIoctlSynchronously(
		WdfDeviceGetIoTarget(FxDevice),
		NULL,
		IOCTL_ACPI_GET_DEVICE_SPECIFIC_DATA,
		&inputMemDesc,
		&outputMemDesc,
		NULL,
		NULL
	);
	if (!NT_SUCCESS(status)) {
		Rt1011Print(
			DEBUG_LEVEL_ERROR,
			DBG_IOCTL,
			"Error getting device data - 0x%x\n",
			status);
		goto Exit;
	}

	if (outputBuffer->Signature != ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE_V1 &&
		outputBuffer->Count < 1 &&
		outputBuffer->Argument->Type != ACPI_METHOD_ARGUMENT_INTEGER &&
		outputBuffer->Argument->DataLength < 1) {
		status = STATUS_ACPI_INVALID_ARGUMENT;
		goto Exit;
	}

	if (property) {
		*property = outputBuffer->Argument->Data[0] & 0xF;
	}

Exit:
	if (inputBuffer) {
		ExFreePoolWithTag(inputBuffer, RT1011_POOL_TAG);
	}
	if (outputMemory != WDF_NO_HANDLE) {
		WdfObjectDelete(outputMemory);
	}
	return status;
}

static NTSTATUS rt1011_calibrate(PRT1011_CONTEXT pDevice, unsigned char cali_flag)
{
	unsigned int value, count = 0, r0[3];
	unsigned int chk_cnt = 50; /* DONT change this */
	unsigned int dc_offset;
	unsigned int r0_integer, r0_factor, format;
	NTSTATUS status = STATUS_SUCCESS;

	rt1011_reg_write(pDevice, RT1011_RESET, 0x0000);
	rt1011_reg_write(pDevice, RT1011_SYSTEM_RESET_3, 0x740f);
	rt1011_reg_write(pDevice, RT1011_SYSTEM_RESET_3, 0x770f);

	/* RC clock */
	rt1011_reg_write(pDevice, RT1011_CLK_2, 0x9400);
	rt1011_reg_write(pDevice, RT1011_PLL_1, 0x0800);
	rt1011_reg_write(pDevice, RT1011_PLL_2, 0x0020);
	rt1011_reg_write(pDevice, RT1011_CLK_DET, 0x0800);

	/* ADC/DAC setting */
	rt1011_reg_write(pDevice, RT1011_ADC_SET_5, 0x0a20);
	rt1011_reg_write(pDevice, RT1011_DAC_SET_2, 0xe232);
	rt1011_reg_write(pDevice, RT1011_ADC_SET_4, 0xc000);

	/* DC detection */
	rt1011_reg_write(pDevice, RT1011_SPK_PRO_DC_DET_1, 0xb00c);
	rt1011_reg_write(pDevice, RT1011_SPK_PRO_DC_DET_2, 0xcccc);

	/* Power */
	rt1011_reg_write(pDevice, RT1011_POWER_1, 0xe0e0);
	rt1011_reg_write(pDevice, RT1011_POWER_3, 0x5003);
	rt1011_reg_write(pDevice, RT1011_POWER_9, 0xa860);
	rt1011_reg_write(pDevice, RT1011_DAC_SET_2, 0xa032);

	/* POW_PLL / POW_BG / POW_BG_MBIAS_LV / POW_V/I */
	rt1011_reg_write(pDevice, RT1011_POWER_2, 0x0007);
	rt1011_reg_write(pDevice, RT1011_POWER_3, 0x5ff7);
	rt1011_reg_write(pDevice, RT1011_A_TEMP_SEN, 0x7f44);
	rt1011_reg_write(pDevice, RT1011_A_TIMING_1, 0x4054);
	rt1011_reg_write(pDevice, RT1011_BAT_GAIN_1, 0x309c);

	/* DC offset from EFUSE */
	rt1011_reg_write(pDevice, RT1011_DC_CALIB_CLASSD_3, 0xcb00);
	rt1011_reg_write(pDevice, RT1011_BOOST_CON_1, 0xe080);
	rt1011_reg_write(pDevice, RT1011_POWER_4, 0x16f2);
	rt1011_reg_write(pDevice, RT1011_POWER_6, 0x36ad);

	/* mixer */
	rt1011_reg_write(pDevice, RT1011_MIXER_1, 0x3f1d);

	/* EFUSE read */
	rt1011_reg_write(pDevice, RT1011_EFUSE_CONTROL_1, 0x0d0a);
	
	LARGE_INTEGER WaitInterval;
	WaitInterval.QuadPart = -10 * 1000 * 30;
	KeDelayExecutionThread(KernelMode, false, &WaitInterval);

	rt1011_reg_read(pDevice, RT1011_EFUSE_ADC_OFFSET_18_16, &value);
	dc_offset = value << 16;
	rt1011_reg_read(pDevice, RT1011_EFUSE_ADC_OFFSET_15_0, &value);
	dc_offset |= (value & 0xffff);
	Rt1011Print(DEBUG_LEVEL_INFO, DBG_INIT,
		"ADC offset=0x%x\n", dc_offset);
	rt1011_reg_read(pDevice, RT1011_EFUSE_DAC_OFFSET_G0_20_16, &value);
	dc_offset = value << 16;
	rt1011_reg_read(pDevice, RT1011_EFUSE_DAC_OFFSET_G0_15_0, &value);
	dc_offset |= (value & 0xffff);
	Rt1011Print(DEBUG_LEVEL_INFO, DBG_INIT,
		"Gain0 offset=0x%x\n", dc_offset);
	rt1011_reg_read(pDevice, RT1011_EFUSE_DAC_OFFSET_G1_20_16, &value);
	dc_offset = value << 16;
	rt1011_reg_read(pDevice, RT1011_EFUSE_DAC_OFFSET_G1_15_0, &value);
	dc_offset |= (value & 0xffff);
	Rt1011Print(DEBUG_LEVEL_INFO, DBG_INIT,
		"Gain1 offset=0x%x\n", dc_offset);

	if (cali_flag) {

		rt1011_reg_write(pDevice, RT1011_ADC_SET_1, 0x2925);
		/* Class D on */
		rt1011_reg_write(pDevice, RT1011_CLASS_D_POS, 0x010e);
		rt1011_reg_write(pDevice,
			RT1011_CLASSD_INTERNAL_SET_1, 0x1701);

		/* STP enable */
		rt1011_reg_write(pDevice, RT1011_SPK_TEMP_PROTECT_0, 0x8000);
		rt1011_reg_write(pDevice, RT1011_SPK_TEMP_PROTECT_7, 0xf000);
		rt1011_reg_write(pDevice, RT1011_SPK_TEMP_PROTECT_4, 0x4040);
		rt1011_reg_write(pDevice, RT1011_SPK_TEMP_PROTECT_0, 0xc000);
		rt1011_reg_write(pDevice, RT1011_SPK_TEMP_PROTECT_6, 0x07c2);

		r0[0] = r0[1] = r0[2] = count = 0;
		while (count < chk_cnt) {
			LARGE_INTEGER WaitInterval;
			WaitInterval.QuadPart = -10 * 1000 * 100;
			KeDelayExecutionThread(KernelMode, false, &WaitInterval);

			rt1011_reg_read(pDevice,
				RT1011_INIT_RECIPROCAL_SYN_24_16, &value);
			r0[count % 3] = value << 16;
			rt1011_reg_read(pDevice,
				RT1011_INIT_RECIPROCAL_SYN_15_0, &value);
			r0[count % 3] |= value;

			if (r0[count % 3] == 0)
				continue;

			count++;

			if (r0[0] == r0[1] && r0[1] == r0[2])
				break;
		}
		if (count > chk_cnt) {
			DbgPrint("Calibrate R0 Failure\n");
			status = STATUS_DEVICE_CONFIGURATION_ERROR;
		}
		else {
			format = 2147483648U; /* 2^24 * 128 */
			r0_integer = format / r0[0] / 128;
			r0_factor = ((format / r0[0] * 100) / 128)
				- (r0_integer * 100);
			pDevice->r0_reg = r0[0];
			pDevice->cali_done = 1;
			DbgPrint("r0 resistance about %d.%02d ohm, reg=0x%X\n",
				r0_integer, r0_factor, r0[0]);
		}
	}

	/* depop */
	rt1011_reg_write(pDevice, RT1011_SPK_TEMP_PROTECT_0, 0x0000);
	
	WaitInterval.QuadPart = -10 * 1000 * 400;
	KeDelayExecutionThread(KernelMode, false, &WaitInterval);

	rt1011_reg_write(pDevice, RT1011_POWER_9, 0xa840);
	rt1011_reg_write(pDevice, RT1011_SPK_TEMP_PROTECT_6, 0x0702);
	rt1011_reg_write(pDevice, RT1011_MIXER_1, 0xffdd);
	rt1011_reg_write(pDevice, RT1011_CLASSD_INTERNAL_SET_1, 0x0701);
	rt1011_reg_write(pDevice, RT1011_DAC_SET_3, 0xe004);
	rt1011_reg_write(pDevice, RT1011_A_TEMP_SEN, 0x7f40);
	rt1011_reg_write(pDevice, RT1011_POWER_1, 0x0000);
	rt1011_reg_write(pDevice, RT1011_POWER_2, 0x0000);
	rt1011_reg_write(pDevice, RT1011_POWER_3, 0x0002);
	rt1011_reg_write(pDevice, RT1011_POWER_4, 0x00f2);

	rt1011_reg_write(pDevice, RT1011_RESET, 0x0000);

	if (cali_flag) {
		if (count <= chk_cnt) {
			rt1011_reg_write(pDevice,
				RT1011_INIT_RECIPROCAL_REG_24_16,
				((r0[0] >> 16) & 0x1ff));
			rt1011_reg_write(pDevice,
				RT1011_INIT_RECIPROCAL_REG_15_0,
				(r0[0] & 0xffff));
			rt1011_reg_write(pDevice,
				RT1011_SPK_TEMP_PROTECT_4, 0x4080);
		}
	}

	return status;
}

static void rt1011_r0_load(PRT1011_CONTEXT pDevice)
{
	if (!pDevice->r0_reg)
		return;

	/* write R0 to register */
	rt1011_reg_write(pDevice, RT1011_INIT_RECIPROCAL_REG_24_16,
		((pDevice->r0_reg >> 16) & 0x1ff));
	rt1011_reg_write(pDevice, RT1011_INIT_RECIPROCAL_REG_15_0,
		(pDevice->r0_reg & 0xffff));
	rt1011_reg_write(pDevice, RT1011_SPK_TEMP_PROTECT_4, 0x4080);

	return;
}

static int rt1011_set_tdm_slot(PRT1011_CONTEXT pDevice,
	unsigned int tx_mask, unsigned int rx_mask, int slots, int slot_width)
{
	UINT16 val = 0, tdm_en = 0, rx_slotnum, tx_slotnum;
	int first_bit, last_bit;
	NTSTATUS status;

	if (rx_mask || tx_mask)
		tdm_en = RT1011_TDM_I2S_DOCK_EN_1;

	switch (slots) {
	case 4:
		val |= RT1011_I2S_TX_4CH;
		val |= RT1011_I2S_RX_4CH;
		break;
	case 6:
		val |= RT1011_I2S_TX_6CH;
		val |= RT1011_I2S_RX_6CH;
		break;
	case 8:
		val |= RT1011_I2S_TX_8CH;
		val |= RT1011_I2S_RX_8CH;
		break;
	case 2:
		break;
	default:
		status = STATUS_INVALID_PARAMETER;
		goto _set_tdm_err_;
	}

	switch (slot_width) {
	case 20:
		val |= RT1011_I2S_CH_TX_LEN_20B;
		val |= RT1011_I2S_CH_RX_LEN_20B;
		break;
	case 24:
		val |= RT1011_I2S_CH_TX_LEN_24B;
		val |= RT1011_I2S_CH_RX_LEN_24B;
		break;
	case 32:
		val |= RT1011_I2S_CH_TX_LEN_32B;
		val |= RT1011_I2S_CH_RX_LEN_32B;
		break;
	case 16:
		break;
	default:
		status = STATUS_INVALID_PARAMETER;
		goto _set_tdm_err_;
	}

	/* Rx slot configuration */
	rx_slotnum = hweight_long(rx_mask);
	if (rx_slotnum > 1 || !rx_slotnum) {
		status = STATUS_INVALID_PARAMETER;
		DbgPrint("too many rx slots or zero slot\n");
		goto _set_tdm_err_;
	}

	first_bit = __ffs(rx_mask);
	switch (first_bit) {
	case 0:
	case 2:
	case 4:
	case 6:
		rt1011_reg_update(pDevice,
			RT1011_CROSS_BQ_SET_1, RT1011_MONO_LR_SEL_MASK,
			RT1011_MONO_L_CHANNEL);
		rt1011_reg_update(pDevice,
			RT1011_TDM1_SET_4,
			RT1011_TDM_I2S_TX_L_DAC1_1_MASK |
			RT1011_TDM_I2S_TX_R_DAC1_1_MASK,
			(first_bit << RT1011_TDM_I2S_TX_L_DAC1_1_SFT) |
			((first_bit + 1) << RT1011_TDM_I2S_TX_R_DAC1_1_SFT));
		break;
	case 1:
	case 3:
	case 5:
	case 7:
		rt1011_reg_update(pDevice,
			RT1011_CROSS_BQ_SET_1, RT1011_MONO_LR_SEL_MASK,
			RT1011_MONO_R_CHANNEL);
		rt1011_reg_update(pDevice,
			RT1011_TDM1_SET_4,
			RT1011_TDM_I2S_TX_L_DAC1_1_MASK |
			RT1011_TDM_I2S_TX_R_DAC1_1_MASK,
			((first_bit - 1) << RT1011_TDM_I2S_TX_L_DAC1_1_SFT) |
			(first_bit << RT1011_TDM_I2S_TX_R_DAC1_1_SFT));
		break;
	default:
		status = STATUS_INVALID_PARAMETER;
		goto _set_tdm_err_;
	}

	/* Tx slot configuration */
	tx_slotnum = hweight_long(tx_mask);
	if (tx_slotnum > 2 || !tx_slotnum) {
		status = STATUS_INVALID_PARAMETER;
		DbgPrint("too many tx slots or zero slot\n");
		goto _set_tdm_err_;
	}

	first_bit = __ffs(tx_mask);
	last_bit = __fls(tx_mask);
	if (last_bit - first_bit > 1) {
		status = STATUS_INVALID_PARAMETER;
		DbgPrint("tx slot location error\n");
		goto _set_tdm_err_;
	}

	if (tx_slotnum == 1) {
		rt1011_reg_update(pDevice, RT1011_TDM1_SET_2,
			RT1011_TDM_I2S_DOCK_ADCDAT_LEN_1_MASK |
			RT1011_TDM_ADCDAT1_DATA_LOCATION, first_bit);
		switch (first_bit) {
		case 1:
			rt1011_reg_update(pDevice,
				RT1011_TDM1_SET_3,
				RT1011_TDM_I2S_RX_ADC1_1_MASK,
				RT1011_TDM_I2S_RX_ADC1_1_LL);
			break;
		case 3:
			rt1011_reg_update(pDevice,
				RT1011_TDM1_SET_3,
				RT1011_TDM_I2S_RX_ADC2_1_MASK,
				RT1011_TDM_I2S_RX_ADC2_1_LL);
			break;
		case 5:
			rt1011_reg_update(pDevice,
				RT1011_TDM1_SET_3,
				RT1011_TDM_I2S_RX_ADC3_1_MASK,
				RT1011_TDM_I2S_RX_ADC3_1_LL);
			break;
		case 7:
			rt1011_reg_update(pDevice,
				RT1011_TDM1_SET_3,
				RT1011_TDM_I2S_RX_ADC4_1_MASK,
				RT1011_TDM_I2S_RX_ADC4_1_LL);
			break;
		case 0:
			rt1011_reg_update(pDevice,
				RT1011_TDM1_SET_3,
				RT1011_TDM_I2S_RX_ADC1_1_MASK, 0);
			break;
		case 2:
			rt1011_reg_update(pDevice,
				RT1011_TDM1_SET_3,
				RT1011_TDM_I2S_RX_ADC2_1_MASK, 0);
			break;
		case 4:
			rt1011_reg_update(pDevice,
				RT1011_TDM1_SET_3,
				RT1011_TDM_I2S_RX_ADC3_1_MASK, 0);
			break;
		case 6:
			rt1011_reg_update(pDevice,
				RT1011_TDM1_SET_3,
				RT1011_TDM_I2S_RX_ADC4_1_MASK, 0);
			break;
		default:
			status = STATUS_INVALID_PARAMETER;
			Rt1011Print(DEBUG_LEVEL_INFO, DBG_INIT,
				"tx slot location error\n");
			goto _set_tdm_err_;
		}
	}
	else if (tx_slotnum == 2) {
		switch (first_bit) {
		case 0:
		case 2:
		case 4:
		case 6:
			rt1011_reg_update(pDevice,
				RT1011_TDM1_SET_2,
				RT1011_TDM_I2S_DOCK_ADCDAT_LEN_1_MASK |
				RT1011_TDM_ADCDAT1_DATA_LOCATION,
				RT1011_TDM_I2S_DOCK_ADCDAT_2CH | first_bit);
			break;
		default:
			status = STATUS_INVALID_PARAMETER;
			Rt1011Print(DEBUG_LEVEL_INFO, DBG_INIT,
				"tx slot location should be paired and start from slot0/2/4/6\n");
			goto _set_tdm_err_;
		}
	}

	rt1011_reg_update(pDevice, RT1011_TDM1_SET_1,
		RT1011_I2S_CH_TX_MASK | RT1011_I2S_CH_RX_MASK |
		RT1011_I2S_CH_TX_LEN_MASK | RT1011_I2S_CH_RX_LEN_MASK, val);
	rt1011_reg_update(pDevice, RT1011_TDM2_SET_1,
		RT1011_I2S_CH_TX_MASK | RT1011_I2S_CH_RX_MASK |
		RT1011_I2S_CH_TX_LEN_MASK | RT1011_I2S_CH_RX_LEN_MASK, val);
	rt1011_reg_update(pDevice, RT1011_TDM1_SET_2,
		RT1011_TDM_I2S_DOCK_EN_1_MASK, tdm_en);
	rt1011_reg_update(pDevice, RT1011_TDM2_SET_2,
		RT1011_TDM_I2S_DOCK_EN_2_MASK, tdm_en);

	rt1011_reg_update(pDevice, RT1011_TDM_TOTAL_SET,
		RT1011_ADCDAT1_PIN_CONFIG | RT1011_ADCDAT2_PIN_CONFIG,
		RT1011_ADCDAT1_OUTPUT | RT1011_ADCDAT2_OUTPUT);

_set_tdm_err_:
	return status;
}

NTSTATUS
StartCodec(
	PRT1011_CONTEXT pDevice
) {
	NTSTATUS status = STATUS_SUCCESS;
	if (!pDevice->SetUID) {
		status = STATUS_ACPI_INVALID_DATA;
		return status;
	}

	
	uint16_t val = 0;
	rt1011_reg_read(pDevice, RT1011_DEVICE_ID, &val);
	if (val != RT1011_DEVICE_ID_NUM) {
		return STATUS_INVALID_DEVICE_STATE;
	}

	struct reg init_list[] = {
		{ RT1011_POWER_9, 0xa840 },

		{ RT1011_ADC_SET_5, 0x0a20 },
		{ RT1011_DAC_SET_2, 0xa032 },

		{ RT1011_SPK_PRO_DC_DET_1, 0xb00c },
		{ RT1011_SPK_PRO_DC_DET_2, 0xcccc },

		{ RT1011_A_TIMING_1, 0x6054 },

		{ RT1011_POWER_7, 0x3e55 },
		{ RT1011_POWER_8, 0x0520 },
		{ RT1011_BOOST_CON_1, 0xe188 },
		{ RT1011_POWER_4, 0x16f2 },

		{ RT1011_CROSS_BQ_SET_1, 0x0004 },
		{ RT1011_SIL_DET, 0xc313 },
		{ RT1011_SINE_GEN_REG_1, 0x0707 },

		{ RT1011_DC_CALIB_CLASSD_3, 0xcb00 },

		{ RT1011_DAC_SET_1, 0xe702 },
		{ RT1011_DAC_SET_3, 0x2004 }
	};

	if (!pDevice->firstInitDone) {
		GetIntegerProperty(pDevice->FxDevice, "realtek,temperature_calib", &pDevice->temperature_calib);
		GetIntegerProperty(pDevice->FxDevice, "realtek,r0_calib", &pDevice->r0_calib);
	}

	{
		UINT16 r0_integer, r0_factor, format;

		if (!pDevice->firstInitDone) {
			if (pDevice->r0_calib)
				rt1011_calibrate(pDevice, 0);
			else
				rt1011_calibrate(pDevice, 1);
			pDevice->firstInitDone = TRUE;
		}

		/*
		 * This flag should reset after booting.
		 * The factory test will do calibration again and use this flag to check
		 * whether the calibration completed
		 */
		pDevice->cali_done = 0;

		status = rt1011_reg_burstWrite(pDevice, init_list, sizeof(init_list) / sizeof(struct reg));
		if (!NT_SUCCESS(status)) {
			return status;
		}

		/* Apply temperature and calibration data from device property */
		if (pDevice->temperature_calib <= 0xff &&
			pDevice->temperature_calib > 0) {
			rt1011_reg_update(pDevice,
				RT1011_STP_INITIAL_RESISTANCE_TEMP, 0x3ff,
				(pDevice->temperature_calib << 2));
		}

		if (pDevice->r0_calib) {
			pDevice->r0_reg = pDevice->r0_calib;

			format = 2147483648U; /* 2^24 * 128 */
			r0_integer = format / pDevice->r0_reg / 128;
			r0_factor = ((format / pDevice->r0_reg * 100) / 128)
				- (r0_integer * 100);
			Rt1011Print(DEBUG_LEVEL_INFO, DBG_INIT,
				"DP r0 resistance about %d.%02d ohm, reg=0x%X\n",
				r0_integer, r0_factor, pDevice->r0_reg);

			rt1011_r0_load(pDevice);
		}

		rt1011_reg_write(pDevice, RT1011_ADC_SET_1, 0x2925);
	}

	//Set PLL
	rt1011_reg_write(pDevice, RT1011_PLL_1, 0x307e);
	rt1011_reg_write(pDevice, RT1011_PLL_2, 0x0008);
	rt1011_reg_write(pDevice, RT1011_CLK_DET, 0x0c40);
	
	//Set daifmt
	rt1011_reg_write(pDevice, RT1011_TDM_TOTAL_SET, 0x0242);
	rt1011_reg_write(pDevice, RT1011_TDM1_SET_1, 0x0522);


	if (pDevice->UID == 0) {
		rt1011_set_tdm_slot(pDevice, 4, 1, 4, 24);
	}
	else if (pDevice->UID == 1) {
		rt1011_set_tdm_slot(pDevice, 8, 2, 4, 24);
	}
	else if (pDevice->UID == 2) {
		rt1011_set_tdm_slot(pDevice, 1, 1, 4, 24);
	}
	else if (pDevice->UID == 3) {
		rt1011_set_tdm_slot(pDevice, 2, 2, 4, 24);
	}

	//Set power mode
	rt1011_reg_write(pDevice, RT1011_POWER_1, 0xe0a0);
	rt1011_reg_write(pDevice, RT1011_POWER_2, 0x0007);
	rt1011_reg_write(pDevice, RT1011_POWER_3, 0x5ff7);

	//Set speaker mode
	rt1011_reg_write(pDevice, RT1011_SPK_TEMP_PROTECT_0, 0xc0cd);
	rt1011_reg_write(pDevice, RT1011_POWER_9, 0xa860);
	rt1011_reg_write(pDevice, RT1011_CLASSD_INTERNAL_SET_1, 0x1701);

	pDevice->DevicePoweredOn = TRUE;
	return status;
}

NTSTATUS
StopCodec(
	PRT1011_CONTEXT pDevice
) {
	NTSTATUS status;

	status = rt1011_reg_write(pDevice, RT1011_RESET, 0);

	pDevice->DevicePoweredOn = FALSE;
	return status;
}

int CsAudioArg2 = 1;

VOID
CSAudioRegisterEndpoint(
	PRT1011_CONTEXT pDevice
) {
	CsAudioArg arg;
	RtlZeroMemory(&arg, sizeof(CsAudioArg));
	arg.argSz = sizeof(CsAudioArg);
	arg.endpointType = CSAudioEndpointTypeSpeaker;
	arg.endpointRequest = CSAudioEndpointRegister;
	ExNotifyCallback(pDevice->CSAudioAPICallback, &arg, &CsAudioArg2);
}

VOID
CsAudioCallbackFunction(
	IN PRT1011_CONTEXT pDevice,
	CsAudioArg* arg,
	PVOID Argument2
) {
	if (!pDevice) {
		return;
	}

	if (Argument2 == &CsAudioArg2) {
		return;
	}

	pDevice->CSAudioManaged = TRUE;

	CsAudioArg localArg;
	RtlZeroMemory(&localArg, sizeof(CsAudioArg));
	RtlCopyMemory(&localArg, arg, min(arg->argSz, sizeof(CsAudioArg)));

	if (localArg.endpointType == CSAudioEndpointTypeDSP && localArg.endpointRequest == CSAudioEndpointRegister) {
		CSAudioRegisterEndpoint(pDevice);
	}
	else if (localArg.endpointType != CSAudioEndpointTypeSpeaker) {
		return;
	}

	if (localArg.endpointRequest == CSAudioEndpointStop) {
		if (pDevice->CSAudioRequestsOn) {
			WdfDeviceResumeIdle(pDevice->FxDevice);
			pDevice->CSAudioRequestsOn = FALSE;
		}
	}
	if (localArg.endpointRequest == CSAudioEndpointStart) {
		if (!pDevice->CSAudioRequestsOn) {
			WdfDeviceStopIdle(pDevice->FxDevice, TRUE);
			pDevice->CSAudioRequestsOn = TRUE;
		}
	}
}

NTSTATUS
OnPrepareHardware(
	_In_  WDFDEVICE     FxDevice,
	_In_  WDFCMRESLIST  FxResourcesRaw,
	_In_  WDFCMRESLIST  FxResourcesTranslated
)
/*++

Routine Description:

This routine caches the SPB resource connection ID.

Arguments:

FxDevice - a handle to the framework device object
FxResourcesRaw - list of translated hardware resources that
the PnP manager has assigned to the device
FxResourcesTranslated - list of raw hardware resources that
the PnP manager has assigned to the device

Return Value:

Status

--*/
{
	PRT1011_CONTEXT pDevice = GetDeviceContext(FxDevice);
	BOOLEAN fSpbResourceFound = FALSE;
	NTSTATUS status = STATUS_INSUFFICIENT_RESOURCES;

	UNREFERENCED_PARAMETER(FxResourcesRaw);

	//
	// Parse the peripheral's resources.
	//

	ULONG resourceCount = WdfCmResourceListGetCount(FxResourcesTranslated);

	for (ULONG i = 0; i < resourceCount; i++)
	{
		PCM_PARTIAL_RESOURCE_DESCRIPTOR pDescriptor;
		UCHAR Class;
		UCHAR Type;

		pDescriptor = WdfCmResourceListGetDescriptor(
			FxResourcesTranslated, i);

		switch (pDescriptor->Type)
		{
		case CmResourceTypeConnection:
			//
			// Look for I2C or SPI resource and save connection ID.
			//
			Class = pDescriptor->u.Connection.Class;
			Type = pDescriptor->u.Connection.Type;
			if (Class == CM_RESOURCE_CONNECTION_CLASS_SERIAL &&
				Type == CM_RESOURCE_CONNECTION_TYPE_SERIAL_I2C)
			{
				if (fSpbResourceFound == FALSE)
				{
					status = STATUS_SUCCESS;
					pDevice->I2CContext.I2cResHubId.LowPart = pDescriptor->u.Connection.IdLowPart;
					pDevice->I2CContext.I2cResHubId.HighPart = pDescriptor->u.Connection.IdHighPart;
					fSpbResourceFound = TRUE;
				}
				else
				{
				}
			}
			break;
		default:
			//
			// Ignoring all other resource types.
			//
			break;
		}
	}

	//
	// An SPB resource is required.
	//

	if (fSpbResourceFound == FALSE)
	{
		status = STATUS_NOT_FOUND;
	}

	status = SpbTargetInitialize(FxDevice, &pDevice->I2CContext);

	if (!NT_SUCCESS(status))
	{
		return status;
	}

	status = GetDeviceUID(FxDevice, &pDevice->UID);
	if (!NT_SUCCESS(status)) {
		return status;
	}
	pDevice->SetUID = TRUE;

	return status;
}

NTSTATUS
OnReleaseHardware(
	_In_  WDFDEVICE     FxDevice,
	_In_  WDFCMRESLIST  FxResourcesTranslated
)
/*++

Routine Description:

Arguments:

FxDevice - a handle to the framework device object
FxResourcesTranslated - list of raw hardware resources that
the PnP manager has assigned to the device

Return Value:

Status

--*/
{
	PRT1011_CONTEXT pDevice = GetDeviceContext(FxDevice);
	NTSTATUS status = STATUS_SUCCESS;

	UNREFERENCED_PARAMETER(FxResourcesTranslated);

	SpbTargetDeinitialize(FxDevice, &pDevice->I2CContext);

	if (pDevice->CSAudioAPICallbackObj) {
		ExUnregisterCallback(pDevice->CSAudioAPICallbackObj);
		pDevice->CSAudioAPICallbackObj = NULL;
	}

	if (pDevice->CSAudioAPICallback) {
		ObfDereferenceObject(pDevice->CSAudioAPICallback);
		pDevice->CSAudioAPICallback = NULL;
	}

	return status;
}

NTSTATUS
OnSelfManagedIoInit(
	_In_
	WDFDEVICE FxDevice
) {
	PRT1011_CONTEXT pDevice = GetDeviceContext(FxDevice);
	NTSTATUS status = STATUS_SUCCESS;

	// CS Audio Callback

	UNICODE_STRING CSAudioCallbackAPI;
	RtlInitUnicodeString(&CSAudioCallbackAPI, L"\\CallBack\\CsAudioCallbackAPI");


	OBJECT_ATTRIBUTES attributes;
	InitializeObjectAttributes(&attributes,
		&CSAudioCallbackAPI,
		OBJ_KERNEL_HANDLE | OBJ_OPENIF | OBJ_CASE_INSENSITIVE | OBJ_PERMANENT,
		NULL,
		NULL
	);
	status = ExCreateCallback(&pDevice->CSAudioAPICallback, &attributes, TRUE, TRUE);
	if (!NT_SUCCESS(status)) {

		return status;
	}

	pDevice->CSAudioAPICallbackObj = ExRegisterCallback(pDevice->CSAudioAPICallback,
		CsAudioCallbackFunction,
		pDevice
	);
	if (!pDevice->CSAudioAPICallbackObj) {

		return STATUS_NO_CALLBACK_ACTIVE;
	}

	CSAudioRegisterEndpoint(pDevice);

	return status;
}

NTSTATUS
OnD0Entry(
	_In_  WDFDEVICE               FxDevice,
	_In_  WDF_POWER_DEVICE_STATE  FxPreviousState
)
/*++

Routine Description:

This routine allocates objects needed by the driver.

Arguments:

FxDevice - a handle to the framework device object
FxPreviousState - previous power state

Return Value:

Status

--*/
{
	UNREFERENCED_PARAMETER(FxPreviousState);

	PRT1011_CONTEXT pDevice = GetDeviceContext(FxDevice);
	NTSTATUS status = STATUS_SUCCESS;

	status = StartCodec(pDevice);

	return status;
}

NTSTATUS
OnD0Exit(
	_In_  WDFDEVICE               FxDevice,
	_In_  WDF_POWER_DEVICE_STATE  FxPreviousState
)
/*++

Routine Description:

This routine destroys objects needed by the driver.

Arguments:

FxDevice - a handle to the framework device object
FxPreviousState - previous power state

Return Value:

Status

--*/
{
	UNREFERENCED_PARAMETER(FxPreviousState);

	PRT1011_CONTEXT pDevice = GetDeviceContext(FxDevice);
	NTSTATUS status = STATUS_SUCCESS;

	status = StopCodec(pDevice);

	return STATUS_SUCCESS;
}

NTSTATUS
Rt1011EvtDeviceAdd(
	IN WDFDRIVER       Driver,
	IN PWDFDEVICE_INIT DeviceInit
)
{
	NTSTATUS                      status = STATUS_SUCCESS;
	WDF_IO_QUEUE_CONFIG           queueConfig;
	WDF_OBJECT_ATTRIBUTES         attributes;
	WDFDEVICE                     device;
	WDFQUEUE                      queue;
	PRT1011_CONTEXT               devContext;

	UNREFERENCED_PARAMETER(Driver);

	PAGED_CODE();

	Rt1011Print(DEBUG_LEVEL_INFO, DBG_PNP,
		"Rt1011EvtDeviceAdd called\n");

	{
		WDF_PNPPOWER_EVENT_CALLBACKS pnpCallbacks;
		WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpCallbacks);

		pnpCallbacks.EvtDevicePrepareHardware = OnPrepareHardware;
		pnpCallbacks.EvtDeviceReleaseHardware = OnReleaseHardware;
		pnpCallbacks.EvtDeviceSelfManagedIoInit = OnSelfManagedIoInit;
		pnpCallbacks.EvtDeviceD0Entry = OnD0Entry;
		pnpCallbacks.EvtDeviceD0Exit = OnD0Exit;

		WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpCallbacks);
	}

	//
	// Setup the device context
	//

	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, RT1011_CONTEXT);

	//
	// Create a framework device object.This call will in turn create
	// a WDM device object, attach to the lower stack, and set the
	// appropriate flags and attributes.
	//

	status = WdfDeviceCreate(&DeviceInit, &attributes, &device);

	if (!NT_SUCCESS(status))
	{
		Rt1011Print(DEBUG_LEVEL_ERROR, DBG_PNP,
			"WdfDeviceCreate failed with status code 0x%x\n", status);

		return status;
	}

	{
		WDF_DEVICE_STATE deviceState;
		WDF_DEVICE_STATE_INIT(&deviceState);

		deviceState.NotDisableable = WdfFalse;
		WdfDeviceSetDeviceState(device, &deviceState);
	}

	WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);

	queueConfig.EvtIoInternalDeviceControl = Rt1011EvtInternalDeviceControl;

	status = WdfIoQueueCreate(device,
		&queueConfig,
		WDF_NO_OBJECT_ATTRIBUTES,
		&queue
	);

	if (!NT_SUCCESS(status))
	{
		Rt1011Print(DEBUG_LEVEL_ERROR, DBG_PNP,
			"WdfIoQueueCreate failed 0x%x\n", status);

		return status;
	}

	//
	// Create manual I/O queue to take care of hid report read requests
	//

	devContext = GetDeviceContext(device);

	devContext->FxDevice = device;
	devContext->firstInitDone = FALSE;

	WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchManual);

	queueConfig.PowerManaged = WdfTrue;

	status = WdfIoQueueCreate(device,
		&queueConfig,
		WDF_NO_OBJECT_ATTRIBUTES,
		&devContext->ReportQueue
	);

	if (!NT_SUCCESS(status))
	{
		Rt1011Print(DEBUG_LEVEL_ERROR, DBG_PNP,
			"WdfIoQueueCreate failed 0x%x\n", status);

		return status;
	}

	WDF_DEVICE_POWER_POLICY_IDLE_SETTINGS IdleSettings;

	WDF_DEVICE_POWER_POLICY_IDLE_SETTINGS_INIT(&IdleSettings, IdleCannotWakeFromS0);
	IdleSettings.IdleTimeoutType = SystemManagedIdleTimeoutWithHint;
	IdleSettings.IdleTimeout = 1000;
	IdleSettings.Enabled = WdfTrue;

	WdfDeviceAssignS0IdleSettings(devContext->FxDevice, &IdleSettings);

	return status;
}

VOID
Rt1011EvtInternalDeviceControl(
	IN WDFQUEUE     Queue,
	IN WDFREQUEST   Request,
	IN size_t       OutputBufferLength,
	IN size_t       InputBufferLength,
	IN ULONG        IoControlCode
)
{
	NTSTATUS            status = STATUS_SUCCESS;
	WDFDEVICE           device;
	PRT1011_CONTEXT     devContext;

	UNREFERENCED_PARAMETER(OutputBufferLength);
	UNREFERENCED_PARAMETER(InputBufferLength);

	device = WdfIoQueueGetDevice(Queue);
	devContext = GetDeviceContext(device);

	switch (IoControlCode)
	{
	default:
		status = STATUS_NOT_SUPPORTED;
		break;
	}

	WdfRequestComplete(Request, status);

	return;
}

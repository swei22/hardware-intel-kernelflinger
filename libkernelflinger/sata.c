/*
 * Copyright (c) 2015, Intel Corporation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *    * Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *    * Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer
 *      in the documentation and/or other materials provided with the
 *      distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * This file defines bootlogic data structures, try to keep it without
 * any external definitions in order to ease export of it.
 */

#include <lib.h>
#include "protocol/AtaPassThru.h"
#include "protocol/Atapi.h"
#include "storage.h"

#define TRIM_SUPPORTED_BIT		0x01
#define BIT5				0x20
#define BIT6				0x40
#define BIT7				0x80
#define ATA_TIMEOUT_NS			30000000
#define BLOCK_SIZE			0x200
#define MAX_SECTOR_PER_RANGE		0xFFFF
#define ATA_CMD_DSM_TRIM_FEATURE	0x1
#define PORT_MULTIPLIER_POS		0x4

typedef struct lba_range_entry {
	UINT16 lba[3];
	UINT16 len;
} __attribute__((packed)) lba_range_entry_t;

static SATA_DEVICE_PATH *get_sata_device_path(EFI_DEVICE_PATH *p)
{
	for (; !IsDevicePathEndType(p); p = NextDevicePathNode(p))
		if (DevicePathType(p) == MESSAGING_DEVICE_PATH
		    && DevicePathSubType(p) == MSG_SATA_DP)
			return (SATA_DEVICE_PATH *)p;

	return NULL;
}

static EFI_STATUS sata_identify_data(EFI_ATA_PASS_THRU_PROTOCOL *ata,
				     SATA_DEVICE_PATH *sata_dp,
				     ATA_IDENTIFY_DATA *identify_data)
{
	EFI_STATUS ret;
	EFI_ATA_STATUS_BLOCK asb;
	EFI_ATA_COMMAND_BLOCK acb = {
		.AtaCommand = ATA_CMD_IDENTIFY_DRIVE,
		.AtaDeviceHead = (UINT8) (BIT7 | BIT6 | BIT5 |
					  (sata_dp->PortMultiplierPortNumber << PORT_MULTIPLIER_POS))
	};
	EFI_ATA_PASS_THRU_COMMAND_PACKET ata_packet = {
		.Asb = &asb,
		.Acb = &acb,
		.Timeout = ATA_TIMEOUT_NS,
		.Protocol = EFI_ATA_PASS_THRU_PROTOCOL_PIO_DATA_IN,
		.Length = EFI_ATA_PASS_THRU_LENGTH_BYTES | EFI_ATA_PASS_THRU_LENGTH_SECTOR_COUNT,
		.InDataBuffer = identify_data,
		.InTransferLength = sizeof(*identify_data)
	};

	ret = uefi_call_wrapper(ata->PassThru, 5, ata,
				sata_dp->HBAPortNumber,
				sata_dp->PortMultiplierPortNumber,
				&ata_packet, NULL);
	if (EFI_ERROR(ret))
		efi_perror(ret, L"Failed to get ATA_IDENTIFY_DATA");

	return ret;
}

static BOOLEAN is_dsm_trim_supported(EFI_ATA_PASS_THRU_PROTOCOL *ata,
				     SATA_DEVICE_PATH *sata_dp,
				     UINT16 *max_dsm_block_nb)
{
	ATA_IDENTIFY_DATA identify_data;
	EFI_STATUS ret;

	ret = sata_identify_data(ata, sata_dp, &identify_data);
	if (EFI_ERROR(ret))
		return FALSE;

	if (!(identify_data.is_data_set_cmd_supported & TRIM_SUPPORTED_BIT)
	    || identify_data.max_no_of_512byte_blocks_per_data_set_cmd == 0) {
		debug(L"This SATA device does support DATA SET MANAGEMENT command");
		return FALSE;
	}

	*max_dsm_block_nb = identify_data.max_no_of_512byte_blocks_per_data_set_cmd;
	return TRUE;
}

/* http://www.t13.org/documents/uploadeddocuments/docs2009/d2015r2-ataatapi_command_set_-_2_acs-2.pdf
 * See. 7.10 DATA SET MANAG EMENT - 06h, DMA
 * See. 4.18.3.2 LBA Range Entry
 */
static EFI_STATUS ata_dsm_trim(EFI_ATA_PASS_THRU_PROTOCOL *ata,
			       SATA_DEVICE_PATH *sata_dp, EFI_LBA start, EFI_LBA end,
			       UINT16 max_dsm_block_nb)
{
	EFI_STATUS ret = EFI_INVALID_PARAMETER;
	EFI_ATA_STATUS_BLOCK asb;
	EFI_ATA_COMMAND_BLOCK acb = {
		.AtaCommand = ATA_CMD_DSM,
		.AtaFeatures = ATA_CMD_DSM_TRIM_FEATURE,
		.AtaDeviceHead = (UINT8) (BIT7 | BIT6 | BIT5 |
					  (sata_dp->PortMultiplierPortNumber << PORT_MULTIPLIER_POS))
	};
	EFI_ATA_PASS_THRU_COMMAND_PACKET ata_packet = {
		.Asb = &asb,
		.Acb = &acb,
		.Timeout = ATA_TIMEOUT_NS,
		.Protocol = EFI_ATA_PASS_THRU_PROTOCOL_PIO_DATA_OUT,
		.Length = EFI_ATA_PASS_THRU_LENGTH_BYTES | EFI_ATA_PASS_THRU_LENGTH_SECTOR_COUNT
	};
	lba_range_entry_t *range, *buf;
	EFI_LBA nr_sectors, nr_ranges, nr_blocks, i, count, left;

	nr_sectors = end - start + 1;
	nr_ranges = nr_sectors / MAX_SECTOR_PER_RANGE;
	if (nr_sectors % MAX_SECTOR_PER_RANGE)
		nr_ranges++;
	nr_blocks = (nr_ranges * sizeof(*range)) / BLOCK_SIZE;
	if ((nr_ranges * sizeof(UINT64)) % BLOCK_SIZE)
		nr_blocks++;

	ret = alloc_aligned((VOID **)&buf, (VOID **)&range,
			    nr_blocks * BLOCK_SIZE, ata->Mode->IoAlign);
	if (EFI_ERROR(ret)) {
		error(L"Failed to allocate DSM LBA Range buffer");
		return ret;
	}

	for (i = 0; start <= end; start += MAX_SECTOR_PER_RANGE, i++) {
		*((UINT64 *)&range[i]) = start;
		left = end - start + 1;
		range[i].len = left < MAX_SECTOR_PER_RANGE ? left : MAX_SECTOR_PER_RANGE;
	}

	for (i = 0; i < nr_blocks; i += max_dsm_block_nb) {
		ata_packet.OutDataBuffer = ((UINT8 *)range) + i * BLOCK_SIZE;

		count = min(nr_blocks - i, max_dsm_block_nb);
		ata_packet.OutTransferLength = count * BLOCK_SIZE;
		acb.AtaSectorCount = count;

		memset(&asb, 0, sizeof(asb));
		ret = uefi_call_wrapper(ata->PassThru, 5, ata,
					sata_dp->HBAPortNumber,
					sata_dp->PortMultiplierPortNumber,
					&ata_packet, NULL);
		if (EFI_ERROR(ret)) {
			efi_perror(ret, L"DATA SET MANAGEMENT command failed");
			goto out;
		}
	}

out:
	FreePool(buf);
	return ret;
}

static EFI_STATUS sata_erase_blocks(EFI_HANDLE handle,
				    __attribute__((unused)) EFI_BLOCK_IO *bio,
				    EFI_LBA start, EFI_LBA end)
{
	EFI_STATUS ret;
	EFI_GUID AtaPassThruProtocolGuid = EFI_ATA_PASS_THRU_PROTOCOL_GUID;
	EFI_DEVICE_PATH *dp;
	EFI_HANDLE ata_handle;
	SATA_DEVICE_PATH *sata_dp;
	EFI_ATA_PASS_THRU_PROTOCOL *ata;
	UINT16 max_dsm_block_nb;

	dp = DevicePathFromHandle(handle);
	if (!dp) {
		error(L"Failed to get device path from handle");
		return EFI_INVALID_PARAMETER;
	}

	sata_dp = (SATA_DEVICE_PATH *)dp;
	ret = uefi_call_wrapper(BS->LocateDevicePath, 3, &AtaPassThruProtocolGuid,
				(EFI_DEVICE_PATH **)&sata_dp, &ata_handle);
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Failed to locate ATA root device");
		return ret;
	}

	sata_dp = get_sata_device_path(dp);
	if (!sata_dp) {
		error(L"Failed to get ATA device path");
		return EFI_NOT_FOUND;
	}

	ret = uefi_call_wrapper(BS->HandleProtocol, 3, ata_handle,
				&AtaPassThruProtocolGuid, (void *)&ata);
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"failed to get ATA protocol");
		return ret;
	}

	if (is_dsm_trim_supported(ata, sata_dp, &max_dsm_block_nb))
		return ata_dsm_trim(ata, sata_dp, start, end, max_dsm_block_nb);

	return EFI_UNSUPPORTED;
}

static EFI_STATUS sata_check_logical_unit(__attribute__((unused)) EFI_DEVICE_PATH *p,
					  logical_unit_t log_unit)
{
	return log_unit == LOGICAL_UNIT_USER ? EFI_SUCCESS : EFI_UNSUPPORTED;
}

static BOOLEAN is_sata(EFI_DEVICE_PATH *p)
{
	return get_sata_device_path(p) != NULL;
}

struct storage STORAGE(STORAGE_SATA) = {
	.erase_blocks = sata_erase_blocks,
	.check_logical_unit = sata_check_logical_unit,
	.probe = is_sata,
	.name = L"SATA"
};


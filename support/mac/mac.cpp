// Mac SCSI family glue — the user_io hooks (see mac.h).

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "../../user_io.h"
#include "../../spi.h"
#include "../../file_io.h"
#include "mac.h"
#include "mac_eth.h"
#include "mac_disk.h"

static char is_core_named(const char *n)
{
	size_t len = strlen(n);
	return !strncasecmp(user_io_get_core_name(0), n, len)
	    || !strncasecmp(user_io_get_core_name(1), n, len);
}

char is_mac_scsi_family()
{
	static int type = 0;
	if (!type)
	{
		type = (is_core_named("maclc") || is_core_named("macplus") || is_core_named("macquadra800")
	    || is_core_named("macppc7300") || is_core_named("pippin")) ? 1 : 2;
		if (type == 1)
		{
			sd_service = mac_sd_service;
			core_poll = mac_poll;
		}
	}
	return type == 1;
}

char is_mac_scsi_optimized()
{
	static int type = 0;
	if (!type)
	{
		type = (is_core_named("macquadra800") || is_core_named("macppc7300") || is_core_named("pippin")) ? 1 : 2;
	}
	return type == 1;
}

char mac_cd_read_keeps_audio()
{
	static int type = 0;
	if (!type) type = is_core_named("pippin") ? 1 : 2;
	return type == 1;
}

// Cores that can hold the Apple IIe card, whose drives sit on hps_io slots 8
// and up: the LC family (MacLC, MacLCII, MacLCIII, MacLC475, MacLC520, ...)
// and the Color Classic family. Prefix match.
static char is_mac_iie_host()
{
	static int type = 0;
	if (!type) type = (is_core_named("maclc") || is_core_named("maccolorclassic")) ? 1 : 2;
	return type == 1;
}

// The generic mount notice is one byte: (1 << index) | RO flag 0x80, so slot 8
// and up truncate to 0, which hps_io takes as a mount of slot 0. IIe-capable
// cores get the same value as a 16-bit word instead. Slots 0-7 see identical
// bits; slot 8 and up land in the high byte, where unmodified hps_io already
// reads them (img_mounted <= io_din[VD:0]). The RO flag stays bit 7, so a
// read-only mount also raises img_mounted[7]; the core must ignore bit 7
// whenever another bit is set.
int mac_mount_notice(int index, int writable)
{
	if (!is_mac_iie_host()) return 0;
	spi_uio_cmd16(UIO_SET_SDSTAT, (uint16_t)((1 << index) | (writable ? 0 : 0x80)));
	return 1;
}

char is_mac_ring_eth()
{
	static int type = 0;
	if (!type)
	{
		type = (is_core_named("macppc7300")) ? 1 : 2;
	}
	return type == 1;
}

#define MAC_TOOLBOX_SLOT    3   // MacLC.sv VD_TOOLBOX
#define MAC_CD_TOOLBOX_SLOT 5   // MacLC.sv VD_CD_TOOLBOX

// Slot availability is not uniform across the family, so the gates split rather
// than sharing one predicate. A wrong slot corrupts another device's stream:
//   MacPlus   — has the CD-ROM slot but NOT the Toolbox slots. Its slot 3 is
//               the second floppy, and it has no slot 5 at all (VDNUM = 5).
static int mac_cd_ok(void)      { return is_mac_scsi_family(); }
static int mac_toolbox_ok(void) { static int macplus = -1; if (macplus < 0) macplus = is_core_named("macplus"); return mac_cd_ok() && !macplus; }

int mac_toolbox_slot()    { return mac_toolbox_ok() ? MAC_TOOLBOX_SLOT    : -1; }
int mac_cdrom_slot()      { return mac_cd_ok()      ? MAC_CDROM_SLOT      : -1; }
int mac_cd_toolbox_slot() { return mac_toolbox_ok() ? MAC_CD_TOOLBOX_SLOT : -1; }

// CD image translation on the CD-ROM slot: CUE/CHD/raw-2352 become a flat
// 2048-byte-sector virtual disc; flat ISO/TOAST stays on the generic path.
int mac_mount_hook(int index, const char *name, fileTYPE *f, int *writable)
{
	mac_disk_flush(index);
	if (index != mac_cdrom_slot()) return 1;

	int r = mac_cdrom_mount(index, name);
	if (r == MAC_CDROM_HANDLED)
	{
		*writable = 0;
		f->size = (int64_t)mac_cdrom_size(index);   // core sees the virtual disc
	}
	else if (r == MAC_CDROM_REJECT)
	{
		FileClose(f);
		return 0;
	}
	return 1;
}

// Announce a Toolbox slot once per core session: the UIO_SET_SDSTAT pulse
// sets img_mounted[slot], which latches the core-side ready flag.
static void announce_slot(int slot)
{
	spi_uio_cmd8(UIO_SET_SDSTAT, (uint8_t)((1 << slot) | 0x80));
}

void mac_poll()
{
	static int tb_inited, cdc_inited;

	int tb_slot = mac_toolbox_slot();
	if (tb_slot >= 0 && (tb_inited || toolbox_shared_ready()))
	{
		if (!tb_inited)
		{
			announce_slot(tb_slot);
			printf("BlueSCSI Toolbox: shared folder \"%s\" on slot %d\n", toolbox_shared_dir(), tb_slot);
			tb_inited = 1;
		}
	}
	else tb_inited = 0;

	// The CD3 folder may appear later: LIST just returns empty until then.
	int cdc_slot = mac_cd_toolbox_slot();
	if (cdc_slot >= 0)
	{
		if (!cdc_inited)
		{
			announce_slot(cdc_slot);
			printf("BlueSCSI CD Changer: control slot %d (folder \"%s\")\n", cdc_slot, cdchanger_dir());
			cdc_inited = 1;
		}
		cdchanger_poll();   // perform any staged SET NEXT CD image remount
	}
	else cdc_inited = 0;

	// The boot repulse belongs to the CD-ROM drive, not to the CD changer: a
	// core can have the drive without the Toolbox (MacPlus), and it still needs
	// the repulse. Gate it on the drive's own slot.
	if (mac_cdrom_slot() >= 0) mac_cdrom_poll();

	mac_eth_poll();
	mac_disk_poll();
}

int mac_cdda_window(int disk, uint32_t lba)
{
	return disk == MAC_CDROM_SLOT && is_mac_scsi_family() &&
	       lba >= MAC_CDROM_AUDIO_BLK && lba < MAC_CDROM_WIN_BASE;
}

int mac_sd_service(int disk, fileTYPE *f, int op, uint64_t lba, int sz, int ack)
{
	lba = (uint32_t)lba;
	if (mac_disk_service(disk, f, op, lba, sz, ack)) return 1;

	static uint8_t buf[4096];
	if (sz > (int)sizeof(buf)) return 0;

	// Toolbox round-trip (file or CD-changer slot): op 2 = the CDB -> run the
	// op and stage the reply; op 1 = status (lba 0) or DataIn (lba 1+k).
	if (disk == mac_toolbox_slot() || disk == mac_cd_toolbox_slot())
	{
		int cdc = (disk == mac_cd_toolbox_slot());
		if (op == 2)
		{
			EnableIO();
			spi_w(UIO_SECTOR_WR | ack);
			spi_block_read(buf, user_io_get_width(), sz);
			DisableIO();
			cdc ? cdchanger_request(lba, buf, sz) : toolbox_request(lba, buf, sz);
		}
		else if (op & 1)
		{
			cdc ? cdchanger_fill(lba, buf, sz) : toolbox_fill(lba, buf, sz);
			EnableIO();
			spi_w(UIO_SECTOR_RD | ack);
			spi_block_write(buf, user_io_get_width(), sz);
			DisableIO();
		}
		else return -1;
		return 1;
	}

	if (disk == mac_cdrom_slot() && is_mac_scsi_optimized() &&
	    lba >= MAC_CDROM_WIN_BASE && lba < MAC_CDROM_TOC_BLK)
	{
		if (op == 2)
		{
			EnableIO();
			spi_w(UIO_SECTOR_WR | ack);
			spi_block_read(buf, user_io_get_width(), sz);
			DisableIO();
			mac_cdrom_command(lba, buf, sz);
		}
		else if (op & 1)
		{
			mac_cdrom_window_fill(lba, buf, sz);
			EnableIO();
			spi_w(UIO_SECTOR_RD | ack);
			spi_block_write(buf, user_io_get_width(), sz);
			DisableIO();
		}
		else return -1;
		return 1;
	}

	// CD slot with translation active: read-only views of the virtual disc
	// (the core ties sd_wr off). Flat images stay on the generic path.
	if (mac_cdrom_active(disk))
	{
		if (op & 1)
		{
			mac_cdrom_fill(disk, lba, buf, sz);
			EnableIO();
			spi_w(UIO_SECTOR_RD | ack);
			spi_block_write(buf, user_io_get_width(), sz);
			DisableIO();
		}
		else return -1;
		return 1;
	}

	return 0;
}

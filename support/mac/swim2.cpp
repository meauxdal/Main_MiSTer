#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "../../file_io.h"
#include "../../user_io.h"
#include "../../spi.h"
#include "swim2.h"

static const int swim2_slot = 6;
static const uint16_t swim2_status = 0x08, swim2_result = 0x09;
static bool eject_pending;
static uint32_t eject_token;

static bool is_swim2_core()
{
	return !strncasecmp(user_io_get_core_name(0), "macquadra605", 12) ||
	       !strncasecmp(user_io_get_core_name(1), "macquadra605", 12) ||
	       !strncasecmp(user_io_get_core_name(0), "maclc475", 8) ||
	       !strncasecmp(user_io_get_core_name(1), "maclc475", 8);
}

struct swim2_request
{
	uint16_t magic, status, slot;
	uint32_t token, sector;
};

static swim2_request read_request(uint16_t command = swim2_status)
{
	swim2_request r;
	r.magic = spi_uio_cmd_cont(command);
	r.status = spi_w(0);
	r.token = spi_w(0);
	r.token |= uint32_t(spi_w(0)) << 16;
	r.sector = spi_w(0);
	r.sector |= uint32_t(spi_w(0)) << 16;
	r.slot = spi_w(0);
	DisableIO();
	return r;
}

static bool control_valid(const swim2_request &r)
{
	return r.magic == 0x5343 && (r.status & 0xffc0) == 0x0100 && r.slot == swim2_slot;
}

static void control_request(uint32_t token, uint16_t action)
{
	spi_uio_cmd_cont(0x0d);
	spi_w(token);
	spi_w(token >> 16);
	spi_w(action);
	DisableIO();
}

bool swim2_can_mount(int disk, bool eject)
{
	if (disk != swim2_slot || !is_swim2_core()) return true;
	swim2_request r = read_request();
	if (r.magic != 0x5357) return true;
	if (r.status == 0x0100 && r.slot == swim2_slot)
	{
		eject_pending = false;
		return true;
	}
	if (eject && (r.status & 0xfff0) == 0x0100 && r.slot == swim2_slot && (r.status & 8))
	{
		swim2_request c = read_request(0x0c);
		if (control_valid(c) && (c.status & 0x21) == 1)
		{
			control_request(c.token, 1);
			eject_token = c.token;
			eject_pending = true;
		}
	}
	return false;
}

void swim2_poll()
{
	if (!is_swim2_core()) { eject_pending = false; return; }
	if (!eject_pending) return;
	swim2_request r = read_request(0x0c);
	if (!control_valid(r) || r.token != eject_token)
	{
		eject_pending = false;
		return;
	}
	if (r.status & 1) return;
	eject_pending = false;
	user_io_file_mount("", swim2_slot);
}

bool swim2_retry()
{
	if (!is_swim2_core()) return false;
	swim2_request r = read_request(0x0c);
	if (!control_valid(r) || (r.status & 0x21) != 1) return false;
	if ((r.status & 0x14) == 0x14) control_request(r.token, 3);
	else if ((r.status & 0x0e) == 0x08) control_request(r.token, 2);
	else return false;
	return true;
}

static bool write_sector(fileTYPE *file, uint32_t lba, const uint8_t *data)
{
	if (!file || !file->filp || file->zip || file->type) return false;
	int fd = fileno(file->filp);
	int flags = fcntl(fd, F_GETFL);
	struct stat64 st;
	if (flags < 0 || (flags & O_ACCMODE) == O_RDONLY || (flags & O_APPEND) ||
	    fstat64(fd, &st) || !S_ISREG(st.st_mode) || st.st_size != file->size ||
	    st.st_size <= 0 || (st.st_size & 511) || lba >= uint64_t(st.st_size) / 512)
		return false;

	clearerr(file->filp);
	__off64_t offset = uint64_t(lba) * 512;
	if (fseeko64(file->filp, offset, SEEK_SET)) return false;
	size_t written = fwrite(data, 1, 512, file->filp);
	int flushed = fflush(file->filp);
	if (written != 512 || flushed || fsync(fd)) return false;
	file->offset = offset + 512;
	return true;
}

static bool read_sector(fileTYPE *file, uint32_t lba, uint8_t *data)
{
	if (!file || !file->filp || file->zip || file->type) return false;
	int fd = fileno(file->filp);
	int flags = fcntl(fd, F_GETFL);
	struct stat64 st;
	if (flags < 0 || (flags & O_ACCMODE) == O_WRONLY ||
	    fstat64(fd, &st) || !S_ISREG(st.st_mode) || st.st_size != file->size ||
	    st.st_size <= 0 || (st.st_size & 511) || lba >= uint64_t(st.st_size) / 512)
		return false;

	clearerr(file->filp);
	__off64_t offset = uint64_t(lba) * 512;
	if (fseeko64(file->filp, offset, SEEK_SET)) return false;
	size_t count = fread(data, 1, 512, file->filp);
	if (count != 512 || ferror(file->filp)) return false;
	file->offset = offset + 512;
	return true;
}

int swim2_sd_service(int disk, fileTYPE *file, int op, uint64_t lba,
                     uint32_t size, int ack, int wide, uint64_t *cache_lba)
{
	if (disk != swim2_slot || (op != 1 && op != 2) || !is_swim2_core()) return 0;

	bool reading = op == 1;
	swim2_request r = read_request(reading ? 0x0a : swim2_status);
	if (r.magic != (reading ? 0x5352 : 0x5357)) return 0;
	// Inconsistent metadata must not fall through to generic file service.
	if ((reading ? r.status != 0x010b : (r.status & 0xfff7) != 0x0103) ||
	    r.slot != swim2_slot || r.sector != lba ||
	    size != 512 || !wide || ack != (swim2_slot << 8)) return 1;

	uint8_t data[512];
	if (cache_lba) *cache_lba = UINT64_MAX;
	bool success = false;
	if (reading)
	{
		success = read_sector(file, r.sector, data);
		if (!success) memset(data, 0, sizeof(data));
	}
	EnableIO();
	spi_w((reading ? UIO_SECTOR_RD : UIO_SECTOR_WR) | ack);
	if (reading) spi_block_write(data, wide, sizeof(data));
	else spi_block_read(data, wide, sizeof(data));
	DisableIO();

	if (!reading) success = write_sector(file, r.sector, data);
	if (!success) printf("SWIM II: sector %u %s failed.\n", r.sector, reading ? "read" : "write/sync");
	spi_uio_cmd_cont(reading ? 0x0b : swim2_result);
	spi_w(r.token);
	spi_w(r.token >> 16);
	spi_w(success ? 0 : 1);
	DisableIO();
	return 1;
}

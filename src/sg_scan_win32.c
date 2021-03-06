/*
 * Copyright (c) 2006-2017 Douglas Gilbert.
 * All rights reserved.
 * Use of this source code is governed by a BSD-style
 * license that can be found in the BSD_LICENSE file.
 */

/*
 * This utility shows the relationship between various device names and
 * volumes in Windows OSes (Windows 2000, 2003, XP and Vista). There is
 * an optional scsi adapter scan.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <getopt.h>
#include <errno.h>

#include "sg_lib.h"
#include "sg_pr2serr.h"

#ifdef _WIN32_WINNT
 #if _WIN32_WINNT < 0x0602
 #undef _WIN32_WINNT
 #define _WIN32_WINNT 0x0602
 #endif
#else
#define _WIN32_WINNT 0x0602
/* claim its W8 */
#endif

#include "sg_pt_win32.h"

static const char * version_str = "1.17 (win32) 20171007";

#define MAX_SCSI_ELEMS 2048
#define MAX_ADAPTER_NUM 128
#define MAX_PHYSICALDRIVE_NUM 1024
#define MAX_CDROM_NUM 512
#define MAX_TAPE_NUM 512
#define MAX_HOLE_COUNT 16

// IOCTL_STORAGE_QUERY_PROPERTY

#define FILE_DEVICE_MASS_STORAGE    0x0000002d
#define IOCTL_STORAGE_BASE          FILE_DEVICE_MASS_STORAGE
#define FILE_ANY_ACCESS             0

// #define METHOD_BUFFERED             0

#define IOCTL_STORAGE_QUERY_PROPERTY \
    CTL_CODE(IOCTL_STORAGE_BASE, 0x0500, METHOD_BUFFERED, FILE_ANY_ACCESS)


#ifndef _DEVIOCTL_
typedef enum _STORAGE_BUS_TYPE {
    BusTypeUnknown      = 0x00,
    BusTypeScsi         = 0x01,
    BusTypeAtapi        = 0x02,
    BusTypeAta          = 0x03,
    BusType1394         = 0x04,
    BusTypeSsa          = 0x05,
    BusTypeFibre        = 0x06,
    BusTypeUsb          = 0x07,
    BusTypeRAID         = 0x08,
    BusTypeiScsi        = 0x09,
    BusTypeSas          = 0x0A,
    BusTypeSata         = 0x0B,
    BusTypeSd           = 0x0C,
    BusTypeMmc          = 0x0D,
    BusTypeVirtual             = 0xE,
    BusTypeFileBackedVirtual   = 0xF,
    BusTypeMax,
    BusTypeMaxReserved  = 0x7F
} STORAGE_BUS_TYPE, *PSTORAGE_BUS_TYPE;

typedef struct _STORAGE_DEVICE_DESCRIPTOR {
    ULONG Version;
    ULONG Size;
    UCHAR DeviceType;
    UCHAR DeviceTypeModifier;
    BOOLEAN RemovableMedia;
    BOOLEAN CommandQueueing;
    ULONG VendorIdOffset;       /* 0 if not available */
    ULONG ProductIdOffset;      /* 0 if not available */
    ULONG ProductRevisionOffset;/* 0 if not available */
    ULONG SerialNumberOffset;   /* -1 if not available ?? */
    STORAGE_BUS_TYPE BusType;
    ULONG RawPropertiesLength;
    UCHAR RawDeviceProperties[1];
} STORAGE_DEVICE_DESCRIPTOR, *PSTORAGE_DEVICE_DESCRIPTOR;
#endif

typedef struct _STORAGE_DEVICE_UNIQUE_IDENTIFIER {
    ULONG  Version;
    ULONG  Size;
    ULONG  StorageDeviceIdOffset;
    ULONG  StorageDeviceOffset;
    ULONG  DriveLayoutSignatureOffset;
} STORAGE_DEVICE_UNIQUE_IDENTIFIER, *PSTORAGE_DEVICE_UNIQUE_IDENTIFIER;

// Use CompareStorageDuids(PSTORAGE_DEVICE_UNIQUE_IDENTIFIER duid1, duid2)
// to test for equality

#ifndef _DEVIOCTL_
typedef enum _STORAGE_QUERY_TYPE {
    PropertyStandardQuery = 0,
    PropertyExistsQuery,
    PropertyMaskQuery,
    PropertyQueryMaxDefined
} STORAGE_QUERY_TYPE, *PSTORAGE_QUERY_TYPE;

typedef enum _STORAGE_PROPERTY_ID {
    StorageDeviceProperty = 0,
    StorageAdapterProperty,
    StorageDeviceIdProperty,
    StorageDeviceUniqueIdProperty,
    StorageDeviceWriteCacheProperty,
    StorageMiniportProperty,
    StorageAccessAlignmentProperty
} STORAGE_PROPERTY_ID, *PSTORAGE_PROPERTY_ID;

typedef struct _STORAGE_PROPERTY_QUERY {
    STORAGE_PROPERTY_ID PropertyId;
    STORAGE_QUERY_TYPE QueryType;
    UCHAR AdditionalParameters[1];
} STORAGE_PROPERTY_QUERY, *PSTORAGE_PROPERTY_QUERY;
#endif


/////////////////////////////////////////////////////////////////////////////

union STORAGE_DEVICE_DESCRIPTOR_DATA {
    STORAGE_DEVICE_DESCRIPTOR desc;
    char raw[256];
};

union STORAGE_DEVICE_UID_DATA {
    STORAGE_DEVICE_UNIQUE_IDENTIFIER desc;
    char raw[1060];
};

struct storage_elem {
    char    name[32];
    char    volume_letters[32];
    int qp_descriptor_valid;
    int qp_uid_valid;
    union STORAGE_DEVICE_DESCRIPTOR_DATA qp_descriptor;
    union STORAGE_DEVICE_UID_DATA qp_uid;
};


static struct storage_elem * storage_arr;
static int next_unused_elem = 0;
static int verbose = 0;

static struct option long_options[] = {
        {"bus", no_argument, 0, 'b'},
        {"help", no_argument, 0, 'h'},
        {"letter", required_argument, 0, 'l'},
        {"verbose", no_argument, 0, 'v'},
        {"scsi", no_argument, 0, 's'},
        {"version", no_argument, 0, 'V'},
        {0, 0, 0, 0},
};


static void
usage()
{
    pr2serr("Usage: sg_scan  [--bus] [--help] [--letter=VL] [--scsi] "
            "[--verbose] [--version]\n");
    pr2serr("       --bus|-b        output bus type\n"
            "       --help|-h       output this usage message then exit\n"
            "       --letter=VL|-l VL    volume letter (e.g. 'F' for F:) "
            "to match\n"
            "       --scsi|-s       used once: show SCSI adapters (tuple) "
            "scan after\n"
            "                       device scan; default: show no "
            "adapters;\n"
            "                       used twice: show only adapaters\n"
            "       --verbose|-v    increase verbosity\n"
            "       --version|-V    print version string and exit\n\n"
            "Scan for storage and related device names\n");
}

static char *
get_err_str(DWORD err, int max_b_len, char * b)
{
    LPVOID lpMsgBuf;
    int k, num, ch;

    if (max_b_len < 2) {
        if (1 == max_b_len)
            b[0] = '\0';
        return b;
    }
    memset(b, 0, max_b_len);
    FormatMessage(
        FORMAT_MESSAGE_ALLOCATE_BUFFER |
        FORMAT_MESSAGE_FROM_SYSTEM,
        NULL, err,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPTSTR) &lpMsgBuf, 0, NULL );
    num = lstrlen((LPCTSTR)lpMsgBuf);
    if (num < 1)
        return b;
    num = (num < max_b_len) ? num : (max_b_len - 1);
    for (k = 0; k < num; ++k) {
        ch = *((LPCTSTR)lpMsgBuf + k);
        if ((ch >= 0x0) && (ch < 0x7f))
            b[k] = ch & 0x7f;
        else
            b[k] = '?';
    }
    return b;
}

static const char *
get_bus_type(int bt)
{
    switch (bt)
    {
    case BusTypeUnknown:
        return "Unkno";
    case BusTypeScsi:
        return "Scsi ";
    case BusTypeAtapi:
        return "Atapi";
    case BusTypeAta:
        return "Ata  ";
    case BusType1394:
        return "1394 ";
    case BusTypeSsa:
        return "Ssa  ";
    case BusTypeFibre:
        return "Fibre";
    case BusTypeUsb:
        return "Usb  ";
    case BusTypeRAID:
        return "RAID ";
    case BusTypeiScsi:
        return "iScsi";
    case BusTypeSas:
        return "Sas  ";
    case BusTypeSata:
        return "Sata ";
    case BusTypeSd:
        return "Sd   ";
    case BusTypeMmc:
        return "Mmc  ";
    case BusTypeVirtual:
        return "Virt ";
    case BusTypeFileBackedVirtual:
        return "FBVir";
    case BusTypeMax:
        return "Max  ";
    default:
        return "_unkn";
    }
}

static int
query_dev_property(HANDLE hdevice,
                   union STORAGE_DEVICE_DESCRIPTOR_DATA * data)
{
    DWORD num_out, err;
    char b[256];
    STORAGE_PROPERTY_QUERY query = {StorageDeviceProperty,
                                    PropertyStandardQuery, {0} };

    memset(data, 0, sizeof(*data));
    if (! DeviceIoControl(hdevice, IOCTL_STORAGE_QUERY_PROPERTY,
                          &query, sizeof(query), data, sizeof(*data),
                          &num_out, NULL)) {
        if (verbose > 2) {
            err = GetLastError();
            pr2serr("  IOCTL_STORAGE_QUERY_PROPERTY(Devprop) failed, "
                    "Error=%u %s\n", (unsigned int)err,
                    get_err_str(err, sizeof(b), b));
        }
        return -ENOSYS;
    }

    if (verbose > 3)
        pr2serr("  IOCTL_STORAGE_QUERY_PROPERTY(DevProp) num_out=%u\n",
                (unsigned int)num_out);
    return 0;
}

static int
query_dev_uid(HANDLE hdevice, union STORAGE_DEVICE_UID_DATA * data)
{
    DWORD num_out, err;
    char b[256];
    STORAGE_PROPERTY_QUERY query = {StorageDeviceUniqueIdProperty,
                                    PropertyStandardQuery, {0} };

    memset(data, 0, sizeof(*data));
    num_out = 0;
    query.QueryType = PropertyExistsQuery;
    if (! DeviceIoControl(hdevice, IOCTL_STORAGE_QUERY_PROPERTY,
                          &query, sizeof(query), NULL, 0, &num_out, NULL)) {
        if (verbose > 2) {
            err = GetLastError();
            pr2serr("  IOCTL_STORAGE_QUERY_PROPERTY(DevUid(exists)) failed, "
                    "Error=%u %s\n", (unsigned int)err,
                    get_err_str(err, sizeof(b), b));
        }
        if (verbose > 3)
            pr2serr("      num_out=%u\n", (unsigned int)num_out);
        /* interpret any error to mean this property doesn't exist */
        return 0;
    }

    query.QueryType = PropertyStandardQuery;
    if (! DeviceIoControl(hdevice, IOCTL_STORAGE_QUERY_PROPERTY,
                          &query, sizeof(query), data, sizeof(*data),
                          &num_out, NULL)) {
        if (verbose > 2) {
            err = GetLastError();
            pr2serr("  IOCTL_STORAGE_QUERY_PROPERTY(DevUid) failed, Error=%u "
                    "%s\n", (unsigned int)err,
                    get_err_str(err, sizeof(b), b));
        }
        return -ENOSYS;
    }
    if (verbose > 3)
        pr2serr("  IOCTL_STORAGE_QUERY_PROPERTY(DevUid) num_out=%u\n",
                (unsigned int)num_out);
    return 0;
}

/* Updates storage_arr based on sep. Returns 1 if update occurred, 0 if
 * no update occured. */
static int
check_devices(const struct storage_elem * sep)
{
    int k, j;
    struct storage_elem * sarr = storage_arr;

    for (k = 0; k < next_unused_elem; ++k, ++sarr) {
        if ('\0' == sarr->name[0])
            continue;
        if (sep->qp_uid_valid && sarr->qp_uid_valid) {
            if (0 == memcmp(&sep->qp_uid, &sarr->qp_uid,
                            sizeof(sep->qp_uid))) {
                for (j = 0; j < (int)sizeof(sep->volume_letters); ++j) {
                    if ('\0' == sarr->volume_letters[j]) {
                        sarr->volume_letters[j] = sep->name[0];
                        break;
                    }
                }
                return 1;
            }
        } else if (sep->qp_descriptor_valid && sarr->qp_descriptor_valid) {
            if (0 == memcmp(&sep->qp_descriptor, &sarr->qp_descriptor,
                            sizeof(sep->qp_descriptor))) {
                for (j = 0; j < (int)sizeof(sep->volume_letters); ++j) {
                    if ('\0' == sarr->volume_letters[j]) {
                        sarr->volume_letters[j] = sep->name[0];
                        break;
                    }
                }
                return 1;
            }
        }
    }
    return 0;
}

static int
enum_scsi_adapters(void)
{
    int k, j;
    int hole_count = 0;
    HANDLE fh;
    ULONG dummy;
    DWORD err;
    BYTE bus;
    BOOL success;
    char adapter_name[64];
    char inqDataBuff[8192];
    PSCSI_ADAPTER_BUS_INFO  ai;
    char b[256];

    for (k = 0; k < MAX_ADAPTER_NUM; ++k) {
        snprintf(adapter_name, sizeof (adapter_name), "\\\\.\\SCSI%d:", k);
        fh = CreateFile(adapter_name, GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                        OPEN_EXISTING, 0, NULL);
        if (fh != INVALID_HANDLE_VALUE) {
            hole_count = 0;
            success = DeviceIoControl(fh, IOCTL_SCSI_GET_INQUIRY_DATA,
                                      NULL, 0, inqDataBuff,
                                      sizeof(inqDataBuff), &dummy, NULL);
            if (success) {
                PSCSI_BUS_DATA pbd;
                PSCSI_INQUIRY_DATA pid;
                int num_lus, off;

                ai = (PSCSI_ADAPTER_BUS_INFO)inqDataBuff;
                for (bus = 0; bus < ai->NumberOfBusses; bus++) {
                    pbd = ai->BusData + bus;
                    num_lus = pbd->NumberOfLogicalUnits;
                    off = pbd->InquiryDataOffset;
                    for (j = 0; j < num_lus; ++j) {
                        if ((off < (int)sizeof(SCSI_ADAPTER_BUS_INFO)) ||
                            (off > ((int)sizeof(inqDataBuff) -
                                    (int)sizeof(SCSI_INQUIRY_DATA))))
                            break;
                        pid = (PSCSI_INQUIRY_DATA)(inqDataBuff + off);
                        snprintf(b, sizeof(b) - 1, "SCSI%d:%d,%d,%d ", k,
                                 pid->PathId, pid->TargetId, pid->Lun);
                        printf("%-15s", b);
                        snprintf(b, sizeof(b) - 1, "claimed=%d pdt=%xh %s ",
                                 pid->DeviceClaimed,
                                 pid->InquiryData[0] % 0x3f,
                                 ((0 == pid->InquiryData[4]) ? "dubious" :
                                                               ""));
                        printf("%-26s", b);
                        printf("%.8s  %.16s  %.4s\n", pid->InquiryData + 8,
                               pid->InquiryData + 16, pid->InquiryData + 32);
                        off = pid->NextInquiryDataOffset;
                    }
                }
            } else {
                err = GetLastError();
                pr2serr("%s: IOCTL_SCSI_GET_INQUIRY_DATA failed err=%u\n\t%s",
                        adapter_name, (unsigned int)err,
                        get_err_str(err, sizeof(b), b));
            }
            CloseHandle(fh);
        } else {
            err = GetLastError();
            if (ERROR_SHARING_VIOLATION == err)
                pr2serr("%s: in use by other process (sharing violation "
                        "[34])\n", adapter_name);
            else if (verbose > 3)
                pr2serr("%s: CreateFile failed err=%u\n\t%s", adapter_name,
                        (unsigned int)err, get_err_str(err, sizeof(b), b));
            if (++hole_count >= MAX_HOLE_COUNT)
                break;
        }
    }
    return 0;
}

static int
enum_volumes(char letter)
{
    int k;
    HANDLE fh;
    char adapter_name[64];
    struct storage_elem tmp_se;

    if (verbose > 2)
        pr2serr("%s: enter\n", __FUNCTION__ );
    for (k = 0; k < 24; ++k) {
        memset(&tmp_se, 0, sizeof(tmp_se));
        snprintf(adapter_name, sizeof (adapter_name), "\\\\.\\%c:", 'C' + k);
        tmp_se.name[0] = 'C' + k;
        fh = CreateFile(adapter_name, GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                        OPEN_EXISTING, 0, NULL);
        if (fh != INVALID_HANDLE_VALUE) {
            if (query_dev_property(fh, &tmp_se.qp_descriptor) < 0)
                pr2serr("%s: query_dev_property failed\n", __FUNCTION__ );
            else
                tmp_se.qp_descriptor_valid = 1;
            if (query_dev_uid(fh, &tmp_se.qp_uid) < 0) {
                if (verbose > 2)
                    pr2serr("%s: query_dev_uid failed\n", __FUNCTION__ );
            } else
                tmp_se.qp_uid_valid = 1;
            if (('\0' == letter) || (letter == tmp_se.name[0]))
                check_devices(&tmp_se);
            CloseHandle(fh);
        }
    }
    return 0;
}

static int
enum_pds(void)
{
    int k;
    int hole_count = 0;
    HANDLE fh;
    DWORD err;
    char adapter_name[64];
    char b[256];
    struct storage_elem tmp_se;

    if (verbose > 2)
        pr2serr("%s: enter\n", __FUNCTION__ );
    for (k = 0; k < MAX_PHYSICALDRIVE_NUM; ++k) {
        memset(&tmp_se, 0, sizeof(tmp_se));
        snprintf(adapter_name, sizeof (adapter_name),
                 "\\\\.\\PhysicalDrive%d", k);
        snprintf(tmp_se.name, sizeof(tmp_se.name), "PD%d", k);
        fh = CreateFile(adapter_name, GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                        OPEN_EXISTING, 0, NULL);
        if (fh != INVALID_HANDLE_VALUE) {
            if (query_dev_property(fh, &tmp_se.qp_descriptor) < 0)
                pr2serr("%s: query_dev_property failed\n", __FUNCTION__ );
            else
                tmp_se.qp_descriptor_valid = 1;
            if (query_dev_uid(fh, &tmp_se.qp_uid) < 0) {
                if (verbose > 2)
                    pr2serr("%s: query_dev_uid failed\n", __FUNCTION__ );
            } else
                tmp_se.qp_uid_valid = 1;
            hole_count = 0;
            memcpy(&storage_arr[next_unused_elem++], &tmp_se, sizeof(tmp_se));
            CloseHandle(fh);
        } else {
            err = GetLastError();
            if (ERROR_SHARING_VIOLATION == err)
                pr2serr("%s: in use by other process (sharing violation "
                        "[34])\n", adapter_name);
            else if (verbose > 3)
                pr2serr("%s: CreateFile failed err=%u\n\t%s", adapter_name,
                        (unsigned int)err, get_err_str(err, sizeof(b), b));
            if (++hole_count >= MAX_HOLE_COUNT)
                break;
        }
    }
    return 0;
}

static int
enum_cdroms(void)
{
    int k;
    int hole_count = 0;
    HANDLE fh;
    DWORD err;
    char adapter_name[64];
    char b[256];
    struct storage_elem tmp_se;

    if (verbose > 2)
        pr2serr("%s: enter\n", __FUNCTION__ );
    for (k = 0; k < MAX_CDROM_NUM; ++k) {
        memset(&tmp_se, 0, sizeof(tmp_se));
        snprintf(adapter_name, sizeof (adapter_name), "\\\\.\\CDROM%d", k);
        snprintf(tmp_se.name, sizeof(tmp_se.name), "CDROM%d", k);
        fh = CreateFile(adapter_name, GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                        OPEN_EXISTING, 0, NULL);
        if (fh != INVALID_HANDLE_VALUE) {
            if (query_dev_property(fh, &tmp_se.qp_descriptor) < 0)
                pr2serr("%s: query_dev_property failed\n", __FUNCTION__ );
            else
                tmp_se.qp_descriptor_valid = 1;
            if (query_dev_uid(fh, &tmp_se.qp_uid) < 0) {
                if (verbose > 2)
                    pr2serr("%s: query_dev_uid failed\n", __FUNCTION__ );
            } else
                tmp_se.qp_uid_valid = 1;
            hole_count = 0;
            memcpy(&storage_arr[next_unused_elem++], &tmp_se, sizeof(tmp_se));
            CloseHandle(fh);
        } else {
            err = GetLastError();
            if (ERROR_SHARING_VIOLATION == err)
                pr2serr("%s: in use by other process (sharing violation "
                        "[34])\n", adapter_name);
            else if (verbose > 3)
                pr2serr("%s: CreateFile failed err=%u\n\t%s", adapter_name,
                        (unsigned int)err, get_err_str(err, sizeof(b), b));
            if (++hole_count >= MAX_HOLE_COUNT)
                break;
        }
    }
    return 0;
}

static int
enum_tapes(void)
{
    int k;
    int hole_count = 0;
    HANDLE fh;
    DWORD err;
    char adapter_name[64];
    char b[256];
    struct storage_elem tmp_se;

    if (verbose > 2)
        pr2serr("%s: enter\n", __FUNCTION__ );
    for (k = 0; k < MAX_TAPE_NUM; ++k) {
        memset(&tmp_se, 0, sizeof(tmp_se));
        snprintf(adapter_name, sizeof (adapter_name), "\\\\.\\TAPE%d", k);
        snprintf(tmp_se.name, sizeof(tmp_se.name), "TAPE%d", k);
        fh = CreateFile(adapter_name, GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                        OPEN_EXISTING, 0, NULL);
        if (fh != INVALID_HANDLE_VALUE) {
            if (query_dev_property(fh, &tmp_se.qp_descriptor) < 0)
                pr2serr("%s: query_dev_property failed\n", __FUNCTION__ );
            else
                tmp_se.qp_descriptor_valid = 1;
            if (query_dev_uid(fh, &tmp_se.qp_uid) < 0) {
                if (verbose > 2)
                    pr2serr("%s: query_dev_uid failed\n", __FUNCTION__ );
            } else
                tmp_se.qp_uid_valid = 1;
            hole_count = 0;
            memcpy(&storage_arr[next_unused_elem++], &tmp_se, sizeof(tmp_se));
            CloseHandle(fh);
        } else {
            err = GetLastError();
            if (ERROR_SHARING_VIOLATION == err)
                pr2serr("%s: in use by other process (sharing violation "
                        "[34])\n", adapter_name);
            else if (verbose > 3)
                pr2serr("%s: CreateFile failed err=%u\n\t%s", adapter_name,
                        (unsigned int)err, get_err_str(err, sizeof(b), b));
            if (++hole_count >= MAX_HOLE_COUNT)
                break;
        }
    }
    return 0;
}

static int
sg_do_wscan(char letter, bool show_bt, int scsi_scan)
{
    int k, j, n;
    struct storage_elem * sp;

    if (scsi_scan < 2) {
        k = enum_pds();
        if (k)
            return k;
        k = enum_cdroms();
        if (k)
            return k;
        k = enum_tapes();
        if (k)
            return k;
        k = enum_volumes(letter);
        if (k)
            return k;

        for (k = 0; k < next_unused_elem; ++k) {
            sp = storage_arr + k;
            if ('\0' == sp->name[0])
                continue;
            printf("%-7s ", sp->name);
            n = strlen(sp->volume_letters);
            if (0 == n)
                printf("        ");
            else if (1 == n)
                printf("[%s]     ", sp->volume_letters);
            else if (2 == n)
                printf("[%s]    ", sp->volume_letters);
            else if (3 == n)
                printf("[%s]   ", sp->volume_letters);
            else if (4 == n)
                printf("[%s]  ", sp->volume_letters);
            else
                printf("[%4s+] ", sp->volume_letters);
            if (sp->qp_descriptor_valid) {
                if (show_bt)
                    printf("<%s>  ",
                           get_bus_type(sp->qp_descriptor.desc.BusType));
                j = sp->qp_descriptor.desc.VendorIdOffset;
                if (j > 0)
                    printf("%s  ", sp->qp_descriptor.raw + j);
                j = sp->qp_descriptor.desc.ProductIdOffset;
                if (j > 0)
                    printf("%s  ", sp->qp_descriptor.raw + j);
                j = sp->qp_descriptor.desc.ProductRevisionOffset;
                if (j > 0)
                    printf("%s  ", sp->qp_descriptor.raw + j);
                j = sp->qp_descriptor.desc.SerialNumberOffset;
                if (j > 0)
                    printf("%s", sp->qp_descriptor.raw + j);
                printf("\n");
                if (verbose > 2)
                    dStrHexErr(sp->qp_descriptor.raw, 144, 0);
            } else
                printf("\n");
            if ((verbose > 3) && sp->qp_uid_valid) {
                printf("  UID valid, in hex:\n");
                dStrHexErr(sp->qp_uid.raw, sizeof(sp->qp_uid.raw), 1);
            }
        }
    }

    if (scsi_scan) {
        if (scsi_scan < 2)
            printf("\n");
        enum_scsi_adapters();
    }
    return 0;
}


int
main(int argc, char * argv[])
{
    bool show_bt = false;
    int c, ret;
    int vol_letter = 0;
    int scsi_scan = 0;

    while (1) {
        int option_index = 0;

        c = getopt_long(argc, argv, "bhHl:svV", long_options,
                        &option_index);
        if (c == -1)
            break;

        switch (c) {
        case 'b':
            show_bt = true;
            break;
        case 'h':
        case '?':
            usage();
            return 0;
        case 'l':
            vol_letter = toupper(optarg[0]);
            if ((vol_letter < 'C') || (vol_letter > 'Z')) {
                pr2serr("'--letter=' expects a letter in the 'C' to 'Z' "
                        "range\n");
                usage();
                return SG_LIB_SYNTAX_ERROR;
            }
            break;
        case 's':
            ++scsi_scan;
            break;
        case 'v':
            ++verbose;
            break;
        case 'V':
            pr2serr("version: %s\n", version_str);
            return 0;
        default:
            pr2serr("unrecognised option code 0x%x ??\n", c);
            usage();
            return SG_LIB_SYNTAX_ERROR;
        }
    }
    if (optind < argc) {
        if (optind < argc) {
            for (; optind < argc; ++optind)
                pr2serr("Unexpected extra argument: %s\n", argv[optind]);
            usage();
            return SG_LIB_SYNTAX_ERROR;
        }
    }

    storage_arr = calloc(sizeof(struct storage_elem) * MAX_SCSI_ELEMS, 1);
    if (storage_arr) {
        ret = sg_do_wscan(vol_letter, show_bt, scsi_scan);
        free(storage_arr);
    } else {
        pr2serr("Failed to allocate storage_arr on heap\n");
        ret = SG_LIB_SYNTAX_ERROR;
    }
    return ret;
}

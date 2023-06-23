#include "DiskAccess.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static SD_Handle sdHandle;
static unsigned long write_pos; // unsigned longs can't handle total possible positions in sd card. Need to switch to a write_sector and write_pos values
static unsigned long read_pos; // same for read
static unsigned int sector_size;
static unsigned int num_sectors;
static unsigned char dirty;
static unsigned long long total_size;
static char* txn_buffer;

static unsigned long soft_read_pos;

// read when there's nothing left to read and write when out of space errors
// change function names to match sdraw

int da_initialize() {
    SD_init();
    soft_read_pos = 0;
    return DISK_SUCCESS;
}

int da_load() {
    int delimiter = 0;
    //int result = 0;

    sdHandle = SD_open(Board_SD0, NULL);
    if (sdHandle == NULL) return DISK_NULL_HANDLE;


    int_fast8_t status = SD_initialize(sdHandle);
    if (status != SD_STATUS_SUCCESS) return DISK_FAILED_INIT;

    sector_size = SD_getSectorSize(sdHandle);
    num_sectors = SD_getNumSectors(sdHandle) - 1;
    txn_buffer = (char *) malloc(sector_size * sizeof(char));
    total_size = sector_size * num_sectors;
    status = SD_read(sdHandle, txn_buffer, 0, 1);
//    print(txn_buffer);
    if (status != SD_STATUS_SUCCESS) {
        return DISK_FAILED_READ;
    }

    int i = 0;
    while (i < sector_size) {
        if (txn_buffer[i] == ':') {
            delimiter = i;
            break;
        }
        ++i;
    }

    if (delimiter) {
        write_pos = atoi(txn_buffer);
        read_pos = atoi(txn_buffer + delimiter + 1);
    }
    else {
        write_pos = 0;
        read_pos = 0;
    }
    cur_sector_num = -1;

    dirty = 0;

    return DISK_SUCCESS;
}

int da_clear() {
    write_pos = 0;
    read_pos = 0;
    memset(txn_buffer, 0, sector_size);
    return DISK_SUCCESS;
}

int da_close() {
    if (da_commit() != DISK_SUCCESS) return DISK_FAILED_WRITE;

    free(txn_buffer);
    SD_close(sdHandle);

    return DISK_SUCCESS;
}

int da_commit() {
    int_fast8_t result;

    if (dirty != 0) {
        result = SD_write(sdHandle, txn_buffer, cur_sector_num+1, 1);
        if (result != SD_STATUS_SUCCESS) return -1;
        dirty = 0;
    }
    cur_sector_num = -1;
    memset(txn_buffer, 0, sector_size);
    System_sprintf(txn_buffer, "%ld:%ld", write_pos, read_pos);
    result = SD_write(sdHandle, txn_buffer, 0, 1);
    if (result != SD_STATUS_SUCCESS) return DISK_FAILED_WRITE;
    return DISK_SUCCESS;
}

int da_get_sector(int sector) {
    int_fast8_t result;
    if (sdHandle == NULL) return DISK_NULL_HANDLE;
    sector = sector % num_sectors;

    if (dirty != 0) {
//        System_sprintf(txn_buffer, "%ld:%ld", write_pos, read_pos);
        result = SD_write(sdHandle, txn_buffer, cur_sector_num+1, 1);
        if (result != SD_STATUS_SUCCESS) return DISK_FAILED_WRITE;
    }

    dirty = 0;
    cur_sector_num = sector;

    return DISK_SUCCESS;
}

int da_get_read_sector(int sector) {
    int_fast8_t result;
    if (sdHandle == NULL) return DISK_NULL_HANDLE;
    sector = sector % num_sectors;

    if (dirty != 0) {
        result = SD_write(sdHandle, txn_buffer, cur_sector_num+1, 1);
        if (result != SD_STATUS_SUCCESS) return DISK_FAILED_WRITE;
        dirty = 0;
    }

    result = SD_read(sdHandle, txn_buffer, sector+1, 1);
    if (result != SD_STATUS_SUCCESS) return DISK_FAILED_READ;
    cur_sector_num = sector;

    return DISK_SUCCESS;
}

int da_write(char* buffer, int size) {
    if (sdHandle == NULL) return DISK_NULL_HANDLE;
    int result = 0;
    //if (size > total_size - da_get_data_size()) return -1;
    int totalWritten = 0;
    while (size > 0) {
        if (write_pos / sector_size != cur_sector_num) {
            result = da_get_read_sector(write_pos / sector_size);
            if (result < 0) {
                write_pos -= totalWritten;
                return result;
            }
        }
        int nwrite = (size > sector_size - (write_pos % sector_size)) ? sector_size - (write_pos % sector_size) : size;
        memcpy(txn_buffer + (write_pos % sector_size), buffer + totalWritten, nwrite);

        dirty = 1;
        write_pos += nwrite;
        totalWritten += nwrite;
        size -= nwrite;
    }
    return DISK_SUCCESS;
    //return result;
}

int da_read(char* buffer, int size) {
    if (sdHandle == NULL) return DISK_NULL_HANDLE;
    int result = 0;
    //if (size > da_get_data_size()) return -1;
    int totalRead = 0;
    while (size > 0) {
        if (read_pos / sector_size != cur_sector_num) {
            result = da_get_read_sector(read_pos / sector_size);
            if (result < 0) {
                read_pos -= totalRead;
                return result;
            }
        }

        int nread = (size > sector_size - (read_pos % sector_size)) ? sector_size - (read_pos % sector_size) : size;
        memcpy(buffer + totalRead, txn_buffer + (read_pos % sector_size), nread);

        read_pos += nread;
        totalRead += nread;
        size -= nread;
    }

    return DISK_SUCCESS;
}

int da_get_data_size() {
    int size = write_pos - read_pos;
    if (size < 0) size = total_size - size;
    return size;
}
int da_get_cur_sector() {
    return cur_sector_num;
}

int da_get_read_pos() {
    return read_pos;
}

int da_get_write_pos() {
    return write_pos;
}
void da_set_write_pos(int position) {
    write_pos = position;
}

int da_get_sector_size() {
    return sector_size;
}

unsigned int da_get_num_sectors(){
    return num_sectors;
}

int da_soft_commit() {
    soft_read_pos = read_pos;
    return read_pos;
}
int da_soft_rollback() {
    read_pos = soft_read_pos;
    return read_pos;
}

char* da_get_transaction_buffer() {
    return txn_buffer;
}

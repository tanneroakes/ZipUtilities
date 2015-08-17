/* zip.c -- IO on .zip files using zlib
   Version 1.1, February 14h, 2010
   part of the MiniZip project - ( http://www.winimage.com/zLibDll/minizip.html )

         Copyright (C) 1998-2010 Gilles Vollant (minizip) ( http://www.winimage.com/zLibDll/minizip.html )

         Modifications for Zip64 support
         Copyright (C) 2009-2010 Mathias Svensson ( http://result42.com )
 
         Modifications for modernization of code
         Copyright (C) 2015 Nolan O'Brien ( http://www.nsprogrammer.com )

         For more info read MiniZip_info.txt

         Changes
   Oct-2009 - Mathias Svensson - Remove old C style function prototypes
   Oct-2009 - Mathias Svensson - Added Zip64 Support when creating new file archives
   Oct-2009 - Mathias Svensson - Did some code cleanup and refactoring to get better overview of some functions.
   Oct-2009 - Mathias Svensson - Added zipRemoveExtraInfoBlock to strip extra field data from its ZIP64 data
                                 It is used when recreting zip archive with RAW when deleting items from a zip.
                                 ZIP64 data is automaticly added to items that needs it, and existing ZIP64 data need to be removed.
   Oct-2009 - Mathias Svensson - Added support for BZIP2 as compression mode (bzip2 lib is required)
   Jan-2010 - back to unzip and minizip 1.0 name scheme, with compatibility layer
   Aug-2015 - Nolan O'Brien - Fix compiler warnings and static analysis warnings
   Aug-2015 - Nolan O'Brien - Code cleanup and style cleanup

*/


#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "zlib.h"
#include "zip.h"

#ifdef STDC
 #include <stddef.h>
 #include <string.h>
 #include <stdlib.h>
#endif

#ifdef NO_ERRNO_H
 extern int errno;
#else
 #include <errno.h>
#endif


#ifndef local
 #define local static
#endif
/* compile with -Dlocal if your debugger can't find static symbols */

#ifndef VERSIONMADEBY
 #define VERSIONMADEBY  (0x0) /* platform depedent */
#endif

#ifndef Z_BUFSIZE
 #define Z_BUFSIZE  (64*1024) //(16384)
#endif

#ifndef Z_MAXFILENAMEINZIP
 #define Z_MAXFILENAMEINZIP (256)
#endif

#ifndef ALLOC
 #define ALLOC(size) (malloc(size))
#endif

#ifndef TRYFREE
 #define TRYFREE(p) {if (p) free(p);}
#endif

/*
#define SIZECENTRALDIRITEM (0x2e)
#define SIZEZIPLOCALHEADER (0x1e)
*/

/* I've found an old Unix (a SunOS 4.1.3_U1) without all SEEK_* defined.... */


// NOT sure that this work on ALL platform
#define MAKEULONG64(a, b) ((ZPOS64_T)(((unsigned long)(a)) | ((ZPOS64_T)((unsigned long)(b))) << 32))

#ifndef DEF_MEM_LEVEL
 #if MAX_MEM_LEVEL >= 8
  #define DEF_MEM_LEVEL (8)
 #else
  #define DEF_MEM_LEVEL MAX_MEM_LEVEL
 #endif
#endif

#define SIZEDATA_INDATABLOCK    (4096 - ( 4 * 4))

#define LOCALHEADERMAGIC        (0x04034b50)
#define CENTRALHEADERMAGIC      (0x02014b50)
#define ENDHEADERMAGIC          (0x06054b50)
#define ZIP64ENDHEADERMAGIC     (0x6064b50)
#define ZIP64ENDLOCHEADERMAGIC  (0x7064b50)

#define FLAG_LOCALHEADER_OFFSET (0x06)
#define CRC_LOCALHEADER_OFFSET  (0x0e)

#define SIZECENTRALHEADER (0x2e) /* 46 */

typedef struct linkedlist_datablock_internal_s
{
  struct linkedlist_datablock_internal_s* next_datablock;
  uLong  avail_in_this_block;
  uLong  filled_in_this_block;
  uLong  unused; /* for future use and alignement */
  unsigned char data[SIZEDATA_INDATABLOCK];
} linkedlist_datablock_internal;

typedef struct linkedlist_data_s
{
    linkedlist_datablock_internal* first_block;
    linkedlist_datablock_internal* last_block;
} linkedlist_data;


typedef struct
{
    z_stream stream;            /* zLib stream structure for inflate */
#ifdef HAVE_BZIP2
    bz_stream bstream;          /* bzLib stream structure for bziped */
#endif

    int  stream_initialised;    /* 1 is stream is initialised */
    uInt pos_in_buffered_data;  /* last written byte in buffered_data */

    ZPOS64_T pos_local_header;     /* offset of the local header of the file
                                     currenty writing */
    char* central_header;       /* central header data for the current file */
    uLong size_centralExtra;
    uLong size_centralheader;   /* size of the central header for cur file */
    uLong size_centralExtraFree; /* Extra bytes allocated to the centralheader but that are not used */
    uLong flag;                 /* flag of the file currently writing */

    int  method;                /* compression method of file currenty wr.*/
    int  raw;                   /* 1 for directly writing raw data */
    Byte buffered_data[Z_BUFSIZE];/* buffer contain compressed data to be writ*/
    uLong dosDate;
    uLong crc32;
    int  encrypt;
    int  zip64;               /* Add ZIP64 extened information in the extra field */
    ZPOS64_T pos_zip64extrainfo;
    ZPOS64_T totalCompressedData;
    ZPOS64_T totalUncompressedData;
#ifndef NOCRYPT
    unsigned long keys[3];     /* keys defining the pseudo-random sequence */
    const unsigned long* pcrc_32_tab;
    int crypt_header_size;
#endif
} curfile64_info;

typedef struct
{
    voidpf filestream;        /* io structore of the zipfile */
    linkedlist_data central_dir;/* datablock with central dir in construction*/
    int  in_opened_file_inzip;  /* 1 if a file in the zip is currently writ.*/
    curfile64_info ci;            /* info on the file curretly writing */

    ZPOS64_T begin_pos;            /* position of the beginning of the zipfile */
    ZPOS64_T add_position_when_writting_offset;
    ZPOS64_T number_entry;

#ifndef NO_ADDFILEINEXISTINGZIP
    char *globalcomment;
#endif

} zip64_internal;


#ifndef NOCRYPT
 #define INCLUDECRYPTINGCODE_IFCRYPTALLOWED
 #include "crypt.h"
#endif

local linkedlist_datablock_internal* allocate_new_datablock()
{
    linkedlist_datablock_internal* ldi;
    ldi = (linkedlist_datablock_internal*)ALLOC(sizeof(linkedlist_datablock_internal));
    if (ldi != NULL) {
        ldi->next_datablock = NULL;
        ldi->filled_in_this_block = 0;
        ldi->avail_in_this_block = SIZEDATA_INDATABLOCK;
    }
    return ldi;
}

local void free_datablock(linkedlist_datablock_internal* ldi)
{
    while (ldi != NULL) {
        linkedlist_datablock_internal* ldinext = ldi->next_datablock;
        TRYFREE(ldi);
        ldi = ldinext;
    }
}

local void init_linkedlist(linkedlist_data* ll)
{
    ll->first_block = ll->last_block = NULL;
}

local void free_linkedlist(linkedlist_data* ll)
{
    free_datablock(ll->first_block);
    ll->first_block = ll->last_block = NULL;
}


local int add_data_in_datablock(linkedlist_data* ll, const void* buf, uLong len)
{
    linkedlist_datablock_internal* ldi;
    const unsigned char* from_copy;

    if (ll == NULL) {
        return ZIP_INTERNALERROR;
    }

    if (ll->last_block == NULL) {
        ll->first_block = ll->last_block = allocate_new_datablock();
        if (ll->first_block == NULL) {
            return ZIP_INTERNALERROR;
        }
    }

    ldi = ll->last_block;
    from_copy = (unsigned char*)buf;

    while (len > 0) {

        if (ldi->avail_in_this_block == 0) {
            ldi->next_datablock = allocate_new_datablock();
            if (ldi->next_datablock == NULL) {
                return ZIP_INTERNALERROR;
            }
            ldi = ldi->next_datablock ;
            ll->last_block = ldi;
        }

        uInt copy_this;
        if (ldi->avail_in_this_block < len) {
            copy_this = (uInt)ldi->avail_in_this_block;
        } else {
            copy_this = (uInt)len;
        }

        unsigned char* to_copy = &(ldi->data[ldi->filled_in_this_block]);

        for (uInt i = 0; i < copy_this; i++) {
            *(to_copy + i)=*(from_copy + i);
        }

        ldi->filled_in_this_block += copy_this;
        ldi->avail_in_this_block -= copy_this;
        from_copy += copy_this ;
        len -= copy_this;

    }
    return ZIP_OK;
}



/****************************************************************************/

#ifndef NO_ADDFILEINEXISTINGZIP
/* ===========================================================================
   Inputs a long in LSB order to the given file
   nbByte == 1, 2 ,4 or 8 (byte, short or long, ZPOS64_T)
*/

local int zip64local_putValue OF((FILE* filestream, ZPOS64_T x, int nbByte));
local int zip64local_putValue(FILE* filestream, ZPOS64_T x, int nbByte)
{
    unsigned char buf[8];
    for (int n = 0; n < nbByte; n++) {
        buf[n] = (unsigned char)(x & 0xff);
        x >>= 8;
    }
    if (x != 0) {
      /* data overflow - hack for ZIP64 (X Roche) */
      for (int n = 0; n < nbByte; n++) {
          buf[n] = 0xff;
      }
    }

    if (fwrite((const char *)buf, 1, (size_t)nbByte, filestream) != (size_t)nbByte) {
        return ZIP_ERRNO;
    } else {
        return ZIP_OK;
    }
}

local void zip64local_putValue_inmemory OF((void* dest, ZPOS64_T x, int nbByte));
local void zip64local_putValue_inmemory(void* dest, ZPOS64_T x, int nbByte)
{
    unsigned char* buf = (unsigned char*)dest;
    for (int n = 0; n < nbByte; n++) {
        buf[n] = (unsigned char)(x & 0xff);
        x >>= 8;
    }

    if (x != 0) {
       /* data overflow - hack for ZIP64 */
       for (int n = 0; n < nbByte; n++) {
          buf[n] = 0xff;
       }
    }
}

/****************************************************************************/


local uLong zip64local_TmzDateToDosDate(const tm_zip* ptm)
{
    uLong year = (uLong)ptm->tm_year;
    if (year >= 1980) {
        year -= 1980;
    } else if (year >= 80) {
        year -= 80;
    }

    uLong dosDate = ((ptm->tm_mday)    + (32 * (ptm->tm_mon + 1)) + (512 * year));
    uLong dosTime = ((ptm->tm_sec / 2) + (32 * ptm->tm_min)       + (2048 * (uLong)ptm->tm_hour));

    return (dosDate << 16) | dosTime;
}

#ifndef BUFREADCOMMENT
#define BUFREADCOMMENT (0x400)
#endif

/*
  Locate the Central directory of a zipfile (at the end, just before
    the global comment)
*/
local ZPOS64_T zip64local_SearchCentralDir OF((voidpf filestream));
local ZPOS64_T zip64local_SearchCentralDir(voidpf filestream)
{
    unsigned char* buf;
    ZPOS64_T uSizeFile;
    ZPOS64_T uBackRead;
    ZPOS64_T uMaxBack = 0xffff; /* maximum size of global comment */
    ZPOS64_T uPosFound = 0;

    if (fseeko(filestream, 0, SEEK_END) != 0) {
        return 0;
    }

    uSizeFile = (ZPOS64_T)ftello(filestream);

    if (uMaxBack > uSizeFile) {
        uMaxBack = uSizeFile;
    }

    buf = (unsigned char*)ALLOC(BUFREADCOMMENT + 4);
    if (buf == NULL) {
        return 0;
    }

    uBackRead = 4;
    while (uBackRead < uMaxBack) {
        uLong uReadSize;
        ZPOS64_T uReadPos;

        if ((uBackRead + BUFREADCOMMENT) > uMaxBack) {
            uBackRead = uMaxBack;
        } else {
            uBackRead += BUFREADCOMMENT;
        }
        uReadPos = uSizeFile - uBackRead;

        uReadSize = ((BUFREADCOMMENT + 4) < (uSizeFile - uReadPos)) ? (BUFREADCOMMENT + 4) : (uLong)(uSizeFile - uReadPos);
        if (fseeko(filestream, (off_t)uReadPos, SEEK_SET) != 0) {
            break;
        }

        if (fread(buf, 1, uReadSize, filestream) != uReadSize) {
            break;
        }

        for (int i = (int)(uReadSize) - 3; (i--) > 0; ) {
            if (((*(buf + i + 0)) == 0x50) &&
                ((*(buf + i + 1)) == 0x4b) &&
                ((*(buf + i + 2)) == 0x05) &&
                ((*(buf + i + 3)) == 0x06)) {
                uPosFound = uReadPos + (uLong)i;
                break;
            }
        }

        if (uPosFound != 0) {
            break;
        }
    }

    TRYFREE(buf);
    return uPosFound;
}

/*
Locate the End of Zip64 Central directory locator and from there find the CD of a zipfile (at the end, just before
the global comment)
*/
local ZPOS64_T zip64local_SearchCentralDir64 OF((voidpf filestream));
local ZPOS64_T zip64local_SearchCentralDir64(voidpf filestream)
{
    unsigned char* buf;
    ZPOS64_T uSizeFile;
    ZPOS64_T uBackRead;
    ZPOS64_T uMaxBack = 0xffff; /* maximum size of global comment */
    ZPOS64_T uPosFound = 0;
    uLong uL;
    ZPOS64_T relativeOffset;

    if (fseeko(filestream, 0, SEEK_SET) != 0) {
        return 0;
    }

    uSizeFile = (ZPOS64_T)ftello(filestream);

    if (uMaxBack > uSizeFile) {
        uMaxBack = uSizeFile;
    }

    buf = (unsigned char*)ALLOC(BUFREADCOMMENT + 4);
    if (buf == NULL) {
        return 0;
    }

    uBackRead = 4;
    while (uBackRead < uMaxBack) {
        uLong uReadSize;
        ZPOS64_T uReadPos;

        if ((uBackRead + BUFREADCOMMENT) > uMaxBack) {
            uBackRead = uMaxBack;
        } else {
            uBackRead += BUFREADCOMMENT;
        }
        uReadPos = uSizeFile - uBackRead ;

        uReadSize = ((BUFREADCOMMENT + 4) < (uSizeFile - uReadPos)) ? (BUFREADCOMMENT + 4) : (uLong)(uSizeFile - uReadPos);
        if (fseeko(filestream, (off_t)uReadPos, SEEK_SET) != 0) {
            break;
        }

        if (fread(buf, 1, uReadSize, filestream) != uReadSize) {
            break;
        }

        for (int i = (int)(uReadSize) - 3; (i--) > 0; ) {
            // Signature "0x07064b50" Zip64 end of central directory locater
            if (((*(buf + i + 0))==0x50) &&
                ((*(buf + i + 1))==0x4b) &&
                ((*(buf + i + 2))==0x06) &&
                ((*(buf + i + 3))==0x07)) {
                uPosFound = uReadPos+(uLong)i;
                break;
            }
        }

        if (uPosFound!=0) {
            break;
        }
    }

    TRYFREE(buf);
    if (uPosFound == 0) {
        return 0;
    }

    /* Zip64 end of central directory locator */
    if (fseeko(filestream, (off_t)uPosFound, SEEK_SET) != 0) {
        return 0;
    }

    /* the signature, already checked */
    if (mz_getLong(filestream, &uL) != ZIP_OK) {
        return 0;
    }

    /* number of the disk with the start of the zip64 end of  central directory */
    if (mz_getLong(filestream, &uL) != ZIP_OK) {
        return 0;
    }
    if (uL != 0) {
        return 0;
    }

    /* relative offset of the zip64 end of central directory record */
    if (mz_getLongLong(filestream, &relativeOffset) != ZIP_OK) {
        return 0;
    }

    /* total number of disks */
    if (mz_getLong(filestream, &uL) != ZIP_OK) {
        return 0;
    }
    if (uL != 1) {
        return 0;
    }

    /* Goto Zip64 end of central directory record */
    if (fseeko(filestream, (off_t)relativeOffset, SEEK_SET) != 0) {
        return 0;
    }

    /* the signature */
    if (mz_getLong(filestream, &uL) != ZIP_OK) {
        return 0;
    }

    // signature of 'Zip64 end of central directory'
    if (uL != 0x06064b50) {
        return 0;
    }

    return relativeOffset;
}

int LoadCentralDirectoryRecord(zip64_internal* pziinit);
int LoadCentralDirectoryRecord(zip64_internal* pziinit)
{
    int err = ZIP_OK;
    ZPOS64_T byte_before_the_zipfile; /* byte before the zipfile, (>0 for sfx)*/

    ZPOS64_T size_central_dir;    /* size of the central directory  */
    ZPOS64_T offset_central_dir;  /* offset of start of central directory */
    ZPOS64_T central_pos;
    uLong tempUnsignedLong;
    ushort tempUnsignedShort;

    uLong number_disk;            /* number of the current dist, used for spaning ZIP, unsupported, always 0 */
    uLong number_disk_with_CD;    /* number the the disk with central dir, used for spaning ZIP, unsupported, always 0 */
    ZPOS64_T number_entry;
    ZPOS64_T number_entry_CD;     /* total number of entries in the central dir (same than number_entry on nospan) */
    ushort versionMadeBy;
    ushort versionNeeded;
    uLong size_comment;

    bool hasZIP64Record = false;

    // check first if we find a ZIP64 record
    central_pos = zip64local_SearchCentralDir64(pziinit->filestream);
    if(central_pos > 0) {
        hasZIP64Record = true;
    } else if(central_pos == 0) {
        central_pos = zip64local_SearchCentralDir(pziinit->filestream);
    }

/* disable to allow appending to empty ZIP archive
    if (central_pos == 0) {
        err = ZIP_ERRNO;
    }
*/

    if (hasZIP64Record) {
        ZPOS64_T sizeEndOfCentralDirectory;
        if (fseeko(pziinit->filestream, (off_t)central_pos, SEEK_SET) != 0) {
            err = ZIP_ERRNO;
        }

        /* the signature, already checked */
        if (mz_getLong(pziinit->filestream, &tempUnsignedLong) != ZIP_OK) {
            err = ZIP_ERRNO;
        }

        /* size of zip64 end of central directory record */
        if (mz_getLongLong(pziinit->filestream, &sizeEndOfCentralDirectory) != ZIP_OK) {
            err = ZIP_ERRNO;
        }

        /* version made by */
        if (mz_getShort(pziinit->filestream, &versionMadeBy) != ZIP_OK) {
            err = ZIP_ERRNO;
        }

        /* version needed to extract */
        if (mz_getShort(pziinit->filestream, &versionNeeded) != ZIP_OK) {
            err = ZIP_ERRNO;
        }

        /* number of this disk */
        if (mz_getLong(pziinit->filestream, &number_disk) != ZIP_OK) {
            err = ZIP_ERRNO;
        }

        /* number of the disk with the start of the central directory */
        if (mz_getLong(pziinit->filestream, &number_disk_with_CD) != ZIP_OK) {
            err = ZIP_ERRNO;
        }

        /* total number of entries in the central directory on this disk */
        if (mz_getLongLong(pziinit->filestream, &number_entry) != ZIP_OK) {
            err = ZIP_ERRNO;
        }

        /* total number of entries in the central directory */
        if (mz_getLongLong(pziinit->filestream, &number_entry_CD) != ZIP_OK) {
            err = ZIP_ERRNO;
        }

        if ((number_entry_CD != number_entry) || (number_disk_with_CD != 0) || (number_disk != 0)) {
            err = ZIP_BADZIPFILE;
        }

        /* size of the central directory */
        if (mz_getLongLong(pziinit->filestream, &size_central_dir) != ZIP_OK) {
            err = ZIP_ERRNO;
        }

        /* offset of start of central directory with respect to the starting disk number */
        if (mz_getLongLong(pziinit->filestream, &offset_central_dir) != ZIP_OK) {
            err = ZIP_ERRNO;
        }

        // TODO..
        // read the comment from the standard central header.
        size_comment = 0;

    } else {

        // Read End of central Directory info
        if (fseeko(pziinit->filestream, (off_t)central_pos, SEEK_SET) != 0) {
            err = ZIP_ERRNO;
        }

        /* the signature, already checked */
        if (mz_getLong(pziinit->filestream, &tempUnsignedLong) != ZIP_OK) {
            err = ZIP_ERRNO;
        }

        /* number of this disk */
        if (mz_getShort(pziinit->filestream, &tempUnsignedShort) != ZIP_OK) {
            err = ZIP_ERRNO;
        }
        number_disk = tempUnsignedShort;

        /* number of the disk with the start of the central directory */
        if (mz_getShort(pziinit->filestream, &tempUnsignedShort) != ZIP_OK) {
            err = ZIP_ERRNO;
        }
        number_disk_with_CD = tempUnsignedShort;

        /* total number of entries in the central dir on this disk */
        number_entry = 0;
        if (mz_getShort(pziinit->filestream, &tempUnsignedShort) != ZIP_OK) {
            err = ZIP_ERRNO;
        } else {
            number_entry = tempUnsignedShort;
        }

        /* total number of entries in the central dir */
        number_entry_CD = 0;
        if (mz_getShort(pziinit->filestream, &tempUnsignedShort) != ZIP_OK) {
            err = ZIP_ERRNO;
        } else {
            number_entry_CD = tempUnsignedShort;
        }

        if ((number_entry_CD != number_entry) || (number_disk_with_CD != 0) || (number_disk != 0)) {
            err = ZIP_BADZIPFILE;
        }

        /* size of the central directory */
        size_central_dir = 0;
        if (mz_getLong(pziinit->filestream, &tempUnsignedLong) != ZIP_OK) {
            err = ZIP_ERRNO;
        } else {
            size_central_dir = tempUnsignedLong;
        }

        /* offset of start of central directory with respect to the starting disk number */
        offset_central_dir = 0;
        if (mz_getLong(pziinit->filestream, &tempUnsignedLong) != ZIP_OK) {
            err = ZIP_ERRNO;
        } else {
            offset_central_dir = tempUnsignedLong;
        }
        
        
        /* zipfile global comment length */
        if (mz_getShort(pziinit->filestream, &tempUnsignedShort) != ZIP_OK) {
            err = ZIP_ERRNO;
        }
        size_comment = tempUnsignedShort;
    }

    if ((central_pos < (offset_central_dir + size_central_dir)) && (err == ZIP_OK)) {
        err = ZIP_BADZIPFILE;
    }

    if (err != ZIP_OK) {
        fclose(pziinit->filestream);
        return ZIP_ERRNO;
    }

    if (size_comment > 0) {
        pziinit->globalcomment = (char*)ALLOC(size_comment + 1);
        if (pziinit->globalcomment) {
            size_comment = fread(pziinit->globalcomment, 1, size_comment, pziinit->filestream);
            pziinit->globalcomment[size_comment] = 0;
        }
    }

    byte_before_the_zipfile = central_pos - (offset_central_dir+size_central_dir);
    pziinit->add_position_when_writting_offset = byte_before_the_zipfile;
    /* Read Central Directory Record */
    {
        ZPOS64_T size_central_dir_to_read = size_central_dir;
        size_t buf_size = SIZEDATA_INDATABLOCK;
        void* buf_read = (void*)ALLOC(buf_size);
        if (0 != fseeko(pziinit->filestream,
                        (off_t)(offset_central_dir + byte_before_the_zipfile),
                        SEEK_SET)) {
            err = ZIP_ERRNO;
        }

        while ((size_central_dir_to_read > 0) && (err == ZIP_OK)) {
            ZPOS64_T read_this = SIZEDATA_INDATABLOCK;
            if (read_this > size_central_dir_to_read) {
                read_this = size_central_dir_to_read;
            }

            if (fread(buf_read, 1, (size_t)read_this, pziinit->filestream) != (size_t)read_this) {
                err = ZIP_ERRNO;
            }

            if (err == ZIP_OK) {
                err = add_data_in_datablock(&pziinit->central_dir, buf_read, (uLong)read_this);
            }

            size_central_dir_to_read -= read_this;
        }
        TRYFREE(buf_read);
    }
    pziinit->begin_pos = byte_before_the_zipfile;
    pziinit->number_entry = number_entry_CD;

    if (fseeko(pziinit->filestream, (off_t)(offset_central_dir + byte_before_the_zipfile), SEEK_SET) != 0) {
        err = ZIP_ERRNO;
    }

    return err;
}


#endif /* !NO_ADDFILEINEXISTINGZIP*/


/************************************************************/
extern zipFile ZEXPORT zipOpen3(const void *pathname, zip_append_status append, const char** globalcomment);
extern zipFile ZEXPORT zipOpen3(const void *pathname, zip_append_status append, const char** globalcomment)
{
    zip64_internal ziinit;
    zip64_internal* zi;
    int err = ZIP_OK;

    mz_fopen_mode mode = mz_fopen_mode_readwritefilter;
    if (append == zip_append_status_create) {
        mode |= mz_fopen_mode_create;
    } else {
        mode |= mz_fopen_mode_existing;
    }

    const char *fmode = mz_fopen_mode_to_str(mode);
    ziinit.filestream = fmode ? fopen(pathname, fmode) : NULL;
    if (ziinit.filestream == NULL) {
        return NULL;
    }

    if (append == zip_append_status_createafter) {
        fseeko(ziinit.filestream, 0, SEEK_END);
    }

    ziinit.begin_pos = (ZPOS64_T)ftello(ziinit.filestream);
    ziinit.in_opened_file_inzip = 0;
    ziinit.ci.stream_initialised = 0;
    ziinit.number_entry = 0;
    ziinit.add_position_when_writting_offset = 0;
    init_linkedlist(&(ziinit.central_dir));

    zi = (zip64_internal*)ALLOC(sizeof(zip64_internal));
    if (zi == NULL) {
        fclose(ziinit.filestream);
        return NULL;
    }

    /* now we add file in a zipfile */
#ifndef NO_ADDFILEINEXISTINGZIP
    ziinit.globalcomment = NULL;
    if (append == zip_append_status_addinzip) {
        // Read and Cache Central Directory Records
        err = LoadCentralDirectoryRecord(&ziinit);
    }

    if (globalcomment) {
        *globalcomment = ziinit.globalcomment;
    }
#endif /* !NO_ADDFILEINEXISTINGZIP*/

    if (err != ZIP_OK) {
#ifndef NO_ADDFILEINEXISTINGZIP
        TRYFREE(ziinit.globalcomment);
#endif /* !NO_ADDFILEINEXISTINGZIP*/
        TRYFREE(zi);
        return NULL;
    } else {
        *zi = ziinit;
        return (zipFile)zi;
    }
}

extern zipFile ZEXPORT zipOpen2(const char *pathname, zip_append_status append, const char** globalcomment)
{
    return zipOpen3(pathname, append, globalcomment);
}

extern zipFile ZEXPORT zipOpen(const char* pathname, zip_append_status append)
{
    return zipOpen2((const void*)pathname, append, NULL);
}

int Write_LocalFileHeader(zip64_internal* zi, const char* filename, uInt size_extrafield_local, const void* extrafield_local);
int Write_LocalFileHeader(zip64_internal* zi, const char* filename, uInt size_extrafield_local, const void* extrafield_local)
{
    /* write the local header */
    int err;
    uInt size_filename = (uInt)strlen(filename);
    uInt size_extrafield = size_extrafield_local;

    err = zip64local_putValue(zi->filestream, (uLong)LOCALHEADERMAGIC, 4);

    if (err == ZIP_OK) {
        if(zi->ci.zip64) {
            err = zip64local_putValue(zi->filestream, (uLong)45, 2); /* version needed to extract */
        } else {
            err = zip64local_putValue(zi->filestream, (uLong)20, 2); /* version needed to extract */
        }
    }

    if (err == ZIP_OK) {
        err = zip64local_putValue(zi->filestream, (uLong)zi->ci.flag, 2);
    }

    if (err == ZIP_OK) {
        err = zip64local_putValue(zi->filestream, (uLong)zi->ci.method, 2);
    }

    if (err == ZIP_OK) {
        err = zip64local_putValue(zi->filestream, (uLong)zi->ci.dosDate, 4);
    }

    // CRC / Compressed size / Uncompressed size will be filled in later and rewritten later
    if (err == ZIP_OK) {
        err = zip64local_putValue(zi->filestream, (uLong)0, 4); /* crc 32, unknown */
    }
    if (err == ZIP_OK) {
        if(zi->ci.zip64) {
            err = zip64local_putValue(zi->filestream, (uLong)0xFFFFFFFF, 4); /* compressed size, unknown */
        } else {
            err = zip64local_putValue(zi->filestream, (uLong)0, 4); /* compressed size, unknown */
        }
    }
    if (err == ZIP_OK) {
        if(zi->ci.zip64) {
            err = zip64local_putValue(zi->filestream, (uLong)0xFFFFFFFF, 4); /* uncompressed size, unknown */
        } else {
            err = zip64local_putValue(zi->filestream, (uLong)0, 4); /* uncompressed size, unknown */
        }
    }

    if (err == ZIP_OK) {
        err = zip64local_putValue(zi->filestream, (uLong)size_filename, 2);
    }

    if(zi->ci.zip64) {
        size_extrafield += 20;
    }

    if (err == ZIP_OK) {
        err = zip64local_putValue(zi->filestream, (uLong)size_extrafield, 2);
    }

    if ((err == ZIP_OK) && (size_filename > 0)) {
        if (fwrite(filename, 1, size_filename, zi->filestream) != size_filename) {
            err = ZIP_ERRNO;
        }
    }

    if ((err == ZIP_OK) && (size_extrafield_local > 0)) {
        if (fwrite(extrafield_local, 1, size_extrafield_local, zi->filestream) != size_extrafield_local) {
            err = ZIP_ERRNO;
        }
    }

    if ((err == ZIP_OK) && (zi->ci.zip64)) {
        // write the Zip64 extended info
        short HeaderID = 1;
        short DataSize = 16;
        ZPOS64_T CompressedSize = 0;
        ZPOS64_T UncompressedSize = 0;

        // Remember position of Zip64 extended info for the local file header.
        // (needed when we update size after done with file)
        zi->ci.pos_zip64extrainfo = (ZPOS64_T)ftello(zi->filestream);

        if (ZIP_OK == (err = zip64local_putValue(zi->filestream, (ZPOS64_T)HeaderID,2))) {
            if (ZIP_OK == (err = zip64local_putValue(zi->filestream, (ZPOS64_T)DataSize,2))) {
                if (ZIP_OK == (err = zip64local_putValue(zi->filestream, (ZPOS64_T)UncompressedSize,8))) {
                    err = zip64local_putValue(zi->filestream, (ZPOS64_T)CompressedSize,8);
                }
            }
        }
        
    }
    
    return err;
}

/*
 NOTE.
 When writing RAW the ZIP64 extended information in extrafield_local and extrafield_global needs to be stripped
 before calling this function it can be done with zipRemoveExtraInfoBlock

 It is not done here because then we need to realloc a new buffer since parameters are 'const' and I want to minimize
 unnecessary allocations.
 */
extern int ZEXPORT zipOpenNewFileInZip4_64(zipFile file,
                                           const char* filename,
                                           const zip_fileinfo* zipfi,
                                           const void* extrafield_local,
                                           uInt size_extrafield_local,
                                           const void* extrafield_global,
                                           uInt size_extrafield_global,
                                           const char* comment,
                                           int method,
                                           int level,
                                           int raw,
                                           int windowBits,
                                           int memLevel,
                                           int strategy,
                                           const char* password,
                                           uLong crcForCrypting,
                                           uLong versionMadeBy,
                                           uLong flagBase,
                                           int zip64)
{
    zip64_internal* zi;
    uInt size_filename;
    uInt size_comment;
    int err = ZIP_OK;

#ifdef NOCRYPT
    if (password != NULL) {
        return ZIP_PARAMERROR;
    }
#endif

    if (file == NULL) {
        return ZIP_PARAMERROR;
    }

#ifdef HAVE_BZIP2
    if ((method != 0) && (method != Z_DEFLATED) && (method != Z_BZIP2ED)) {
        return ZIP_PARAMERROR;
    }
#else
    if ((method != 0) && (method != Z_DEFLATED)) {
      return ZIP_PARAMERROR;
    }
#endif

    zi = (zip64_internal*)file;

    if (zi->in_opened_file_inzip == 1) {
        err = zipCloseFileInZip(file);
        if (err != ZIP_OK) {
            return err;
        }
    }

    if (filename == NULL) {
        filename = "-";
    }

    if (comment == NULL) {
        size_comment = 0;
    } else {
        size_comment = (uInt)strlen(comment);
    }

    size_filename = (uInt)strlen(filename);

    if (zipfi == NULL) {
        zi->ci.dosDate = 0;
    } else {
        if (zipfi->dosDate != 0) {
            zi->ci.dosDate = zipfi->dosDate;
        } else {
          zi->ci.dosDate = zip64local_TmzDateToDosDate(&zipfi->tmz_date);
        }
    }

    zi->ci.flag = flagBase;
    if ((level == 8) || (level == 9)) {
        zi->ci.flag |= 2;
    }
    if (level == 2) {
        zi->ci.flag |= 4;
    }
    if (level == 1) {
        zi->ci.flag |= 6;
    }
    if (password != NULL) {
        zi->ci.flag |= 1;
    }

    zi->ci.crc32 = 0;
    zi->ci.method = method;
    zi->ci.encrypt = 0;
    zi->ci.stream_initialised = 0;
    zi->ci.pos_in_buffered_data = 0;
    zi->ci.raw = raw;
    zi->ci.pos_local_header = (ZPOS64_T)ftello(zi->filestream);

    zi->ci.size_centralheader = SIZECENTRALHEADER + size_filename + size_extrafield_global + size_comment;
    zi->ci.size_centralExtraFree = 32; // Extra space we have reserved in case we need to add ZIP64 extra info data

    zi->ci.central_header = (char*)ALLOC((uInt)zi->ci.size_centralheader + zi->ci.size_centralExtraFree);

    zi->ci.size_centralExtra = size_extrafield_global;
    zip64local_putValue_inmemory(zi->ci.central_header, (uLong)CENTRALHEADERMAGIC, 4);
    /* version info */
    zip64local_putValue_inmemory(zi->ci.central_header +  4, (uLong)versionMadeBy, 2);
    zip64local_putValue_inmemory(zi->ci.central_header +  6, (uLong)20, 2);
    zip64local_putValue_inmemory(zi->ci.central_header +  8, (uLong)zi->ci.flag, 2);
    zip64local_putValue_inmemory(zi->ci.central_header + 10, (uLong)zi->ci.method, 2);
    zip64local_putValue_inmemory(zi->ci.central_header + 12, (uLong)zi->ci.dosDate, 4);
    zip64local_putValue_inmemory(zi->ci.central_header + 16, (uLong)0, 4); /*crc*/
    zip64local_putValue_inmemory(zi->ci.central_header + 20, (uLong)0, 4); /*compr size*/
    zip64local_putValue_inmemory(zi->ci.central_header + 24, (uLong)0, 4); /*uncompr size*/
    zip64local_putValue_inmemory(zi->ci.central_header + 28, (uLong)size_filename, 2);
    zip64local_putValue_inmemory(zi->ci.central_header + 30, (uLong)size_extrafield_global, 2);
    zip64local_putValue_inmemory(zi->ci.central_header + 32, (uLong)size_comment, 2);
    zip64local_putValue_inmemory(zi->ci.central_header + 34, (uLong)0, 2); /*disk nm start*/

    if (zipfi == NULL) {
        zip64local_putValue_inmemory(zi->ci.central_header + 36, (ushort)0, 2);
    } else {
        zip64local_putValue_inmemory(zi->ci.central_header + 36, zipfi->internal_fa, 2);
    }

    if (zipfi == NULL) {
        zip64local_putValue_inmemory(zi->ci.central_header + 38, (uLong)0, 4);
    } else {
        zip64local_putValue_inmemory(zi->ci.central_header + 38, zipfi->external_fa, 4);
    }

    if(zi->ci.pos_local_header >= 0xffffffff) {
        zip64local_putValue_inmemory(zi->ci.central_header + 42, (uLong)0xffffffff, 4);
    } else {
        zip64local_putValue_inmemory(zi->ci.central_header + 42, (uLong)zi->ci.pos_local_header - (uLong)zi->add_position_when_writting_offset, 4);
    }

    for (uInt i = 0; i < size_filename; i++) {
        *(zi->ci.central_header + SIZECENTRALHEADER + i) = *(filename + i);
    }

    for (uInt i = 0; i < size_extrafield_global; i++) {
        *(zi->ci.central_header + SIZECENTRALHEADER + size_filename + i) = *(((const char*)extrafield_global) + i);
    }

    for (uInt i = 0; i < size_comment; i++) {
        *(zi->ci.central_header + SIZECENTRALHEADER + size_filename + size_extrafield_global + i) = *(comment + i);
    }

    if (zi->ci.central_header == NULL) {
        return ZIP_INTERNALERROR;
    }

    zi->ci.zip64 = zip64;
    zi->ci.totalCompressedData = 0;
    zi->ci.totalUncompressedData = 0;
    zi->ci.pos_zip64extrainfo = 0;

    err = Write_LocalFileHeader(zi, filename, size_extrafield_local, extrafield_local);

#ifdef HAVE_BZIP2
    zi->ci.bstream.avail_in = (uInt)0;
    zi->ci.bstream.avail_out = (uInt)Z_BUFSIZE;
    zi->ci.bstream.next_out = (char*)zi->ci.buffered_data;
    zi->ci.bstream.total_in_hi32 = 0;
    zi->ci.bstream.total_in_lo32 = 0;
    zi->ci.bstream.total_out_hi32 = 0;
    zi->ci.bstream.total_out_lo32 = 0;
#endif

    zi->ci.stream.avail_in = (uInt)0;
    zi->ci.stream.avail_out = (uInt)Z_BUFSIZE;
    zi->ci.stream.next_out = zi->ci.buffered_data;
    zi->ci.stream.total_in = 0;
    zi->ci.stream.total_out = 0;
    zi->ci.stream.data_type = Z_BINARY;

#ifdef HAVE_BZIP2
    if ((err == ZIP_OK) && (zi->ci.method == Z_DEFLATED || zi->ci.method == Z_BZIP2ED) && (!zi->ci.raw)) {
#else
    if ((err == ZIP_OK) && (zi->ci.method == Z_DEFLATED) && (!zi->ci.raw)) {
#endif
        if(zi->ci.method == Z_DEFLATED) {
            zi->ci.stream.zalloc = (alloc_func)0;
            zi->ci.stream.zfree = (free_func)0;
            zi->ci.stream.opaque = (voidpf)0;

            if (windowBits > 0) {
                windowBits = -windowBits;
            }

            err = deflateInit2(&zi->ci.stream, level, Z_DEFLATED, windowBits, memLevel, strategy);

            if (err == Z_OK) {
                zi->ci.stream_initialised = Z_DEFLATED;
            }
        } else if(zi->ci.method == Z_BZIP2ED) {
#ifdef HAVE_BZIP2
            // Init BZip stuff here
            zi->ci.bstream.bzalloc = 0;
            zi->ci.bstream.bzfree = 0;
            zi->ci.bstream.opaque = (voidpf)0;

            err = BZ2_bzCompressInit(&zi->ci.bstream, level, 0, 35);
            if (err == BZ_OK) {
                zi->ci.stream_initialised = Z_BZIP2ED;
            }
#endif
        }

    }

#ifndef NOCRYPT
    zi->ci.crypt_header_size = 0;
    if ((err == Z_OK) && (password != NULL)) {
        unsigned char bufHead[RAND_HEAD_LEN];
        unsigned int sizeHead;
        zi->ci.encrypt = 1;
        zi->ci.pcrc_32_tab = get_crc_table();
        /*init_keys(password,zi->ci.keys,zi->ci.pcrc_32_tab);*/

        sizeHead=(unsigned int)crypthead(password,
                                         bufHead,
                                         RAND_HEAD_LEN,
                                         zi->ci.keys,
                                         zi->ci.pcrc_32_tab,
                                         crcForCrypting);
        zi->ci.crypt_header_size = (int)sizeHead;

        if (fwrite(bufHead, 1, sizeHead, zi->filestream) != sizeHead) {
            err = ZIP_ERRNO;
        }
    }
#endif // NOCRYPT
    
    if (err == Z_OK) {
        zi->in_opened_file_inzip = 1;
    }

    return err;
}

extern int ZEXPORT zipOpenNewFileInZip4(zipFile file,
                                        const char* filename,
                                        const zip_fileinfo* zipfi,
                                        const void* extrafield_local,
                                        uInt size_extrafield_local,
                                        const void* extrafield_global,
                                        uInt size_extrafield_global,
                                        const char* comment,
                                        int method,
                                        int level,
                                        int raw,
                                        int windowBits,
                                        int memLevel,
                                        int strategy,
                                        const char* password,
                                        uLong crcForCrypting,
                                        uLong versionMadeBy,
                                        uLong flagBase)
{
    return zipOpenNewFileInZip4_64(file,
                                   filename,
                                   zipfi,
                                   extrafield_local,
                                   size_extrafield_local,
                                   extrafield_global,
                                   size_extrafield_global,
                                   comment,
                                   method,
                                   level,
                                   raw,
                                   windowBits,
                                   memLevel,
                                   strategy,
                                   password,
                                   crcForCrypting,
                                   versionMadeBy,
                                   flagBase,
                                   0);
}

extern int ZEXPORT zipOpenNewFileInZip3(zipFile file,
                                        const char* filename,
                                        const zip_fileinfo* zipfi,
                                        const void* extrafield_local,
                                        uInt size_extrafield_local,
                                        const void* extrafield_global,
                                        uInt size_extrafield_global,
                                        const char* comment,
                                        int method,
                                        int level,
                                        int raw,
                                        int windowBits,
                                        int memLevel,
                                        int strategy,
                                        const char* password,
                                        uLong crcForCrypting)
{
    return zipOpenNewFileInZip4_64(file,
                                   filename,
                                   zipfi,
                                   extrafield_local,
                                   size_extrafield_local,
                                   extrafield_global,
                                   size_extrafield_global,
                                   comment,
                                   method,
                                   level,
                                   raw,
                                   windowBits,
                                   memLevel,
                                   strategy,
                                   password,
                                   crcForCrypting,
                                   VERSIONMADEBY,
                                   0,
                                   0);
}

extern int ZEXPORT zipOpenNewFileInZip3_64(zipFile file,
                                           const char* filename,
                                           const zip_fileinfo* zipfi,
                                           const void* extrafield_local,
                                           uInt size_extrafield_local,
                                           const void* extrafield_global,
                                           uInt size_extrafield_global,
                                           const char* comment,
                                           int method,
                                           int level,
                                           int raw,
                                           int windowBits,
                                           int memLevel,
                                           int strategy,
                                           const char* password,
                                           uLong crcForCrypting,
                                           int zip64)
{
    return zipOpenNewFileInZip4_64(file,
                                   filename,
                                   zipfi,
                                   extrafield_local,
                                   size_extrafield_local,
                                   extrafield_global,
                                   size_extrafield_global,
                                   comment,
                                   method,
                                   level,
                                   raw,
                                   windowBits,
                                   memLevel,
                                   strategy,
                                   password,
                                   crcForCrypting,
                                   VERSIONMADEBY,
                                   0,
                                   zip64);
}

extern int ZEXPORT zipOpenNewFileInZip2(zipFile file,
                                        const char* filename,
                                        const zip_fileinfo* zipfi,
                                        const void* extrafield_local,
                                        uInt size_extrafield_local,
                                        const void* extrafield_global,
                                        uInt size_extrafield_global,
                                        const char* comment,
                                        int method,
                                        int level,
                                        int raw)
{
    return zipOpenNewFileInZip4_64(file,
                                   filename,
                                   zipfi,
                                   extrafield_local,
                                   size_extrafield_local,
                                   extrafield_global,
                                   size_extrafield_global,
                                   comment,
                                   method,
                                   level,
                                   raw,
                                   -MAX_WBITS,
                                   DEF_MEM_LEVEL,
                                   Z_DEFAULT_STRATEGY,
                                   NULL,
                                   0,
                                   VERSIONMADEBY,
                                   0,
                                   0);
}

extern int ZEXPORT zipOpenNewFileInZip2_64(zipFile file,
                                           const char* filename,
                                           const zip_fileinfo* zipfi,
                                           const void* extrafield_local,
                                           uInt size_extrafield_local,
                                           const void* extrafield_global,
                                           uInt size_extrafield_global,
                                           const char* comment,
                                           int method,
                                           int level,
                                           int raw,
                                           int zip64)
{
    return zipOpenNewFileInZip4_64(file,
                                   filename,
                                   zipfi,
                                   extrafield_local,
                                   size_extrafield_local,
                                   extrafield_global,
                                   size_extrafield_global,
                                   comment,
                                   method,
                                   level,
                                   raw,
                                   -MAX_WBITS,
                                   DEF_MEM_LEVEL,
                                   Z_DEFAULT_STRATEGY,
                                   NULL,
                                   0,
                                   VERSIONMADEBY,
                                   0,
                                   zip64);
}

extern int ZEXPORT zipOpenNewFileInZip64(zipFile file,
                                         const char* filename,
                                         const zip_fileinfo* zipfi,
                                         const void* extrafield_local,
                                         uInt size_extrafield_local,
                                         const void*extrafield_global,
                                         uInt size_extrafield_global,
                                         const char* comment,
                                         int method,
                                         int level,
                                         int zip64)
{
    return zipOpenNewFileInZip4_64(file,
                                   filename,
                                   zipfi,
                                   extrafield_local,
                                   size_extrafield_local,
                                   extrafield_global,
                                   size_extrafield_global,
                                   comment,
                                   method,
                                   level,
                                   0,
                                   -MAX_WBITS,
                                   DEF_MEM_LEVEL,
                                   Z_DEFAULT_STRATEGY,
                                   NULL,
                                   0,
                                   VERSIONMADEBY,
                                   0,
                                   zip64);
}

extern int ZEXPORT zipOpenNewFileInZip(zipFile file,
                                       const char* filename,
                                       const zip_fileinfo* zipfi,
                                       const void* extrafield_local,
                                       uInt size_extrafield_local,
                                       const void* extrafield_global,
                                       uInt size_extrafield_global,
                                       const char* comment,
                                       int method,
                                       int level)
{
    return zipOpenNewFileInZip4_64(file,
                                   filename,
                                   zipfi,
                                   extrafield_local,
                                   size_extrafield_local,
                                   extrafield_global,
                                   size_extrafield_global,
                                   comment,
                                   method,
                                   level,
                                   0,
                                   -MAX_WBITS,
                                   DEF_MEM_LEVEL,
                                   Z_DEFAULT_STRATEGY,
                                   NULL,
                                   0,
                                   VERSIONMADEBY,
                                   0,
                                   0);
}

local int zip64FlushWriteBuffer(zip64_internal* zi)
{
    int err = ZIP_OK;

    if (zi->ci.encrypt != 0) {
#ifndef NOCRYPT
        for (uInt i = 0; i < zi->ci.pos_in_buffered_data; i++) {
            zi->ci.buffered_data[i] = (Byte)zencode(zi->ci.keys, zi->ci.pcrc_32_tab, zi->ci.buffered_data[i]);
        }
#endif
    }

    if (fwrite(zi->ci.buffered_data, 1, zi->ci.pos_in_buffered_data, zi->filestream) != zi->ci.pos_in_buffered_data) {
        err = ZIP_ERRNO;
    }

    zi->ci.totalCompressedData += zi->ci.pos_in_buffered_data;

#ifdef HAVE_BZIP2
    if(zi->ci.method == Z_BZIP2ED) {
      zi->ci.totalUncompressedData += zi->ci.bstream.total_in_lo32;
      zi->ci.bstream.total_in_lo32 = 0;
      zi->ci.bstream.total_in_hi32 = 0;
    } else
#endif
    {
      zi->ci.totalUncompressedData += zi->ci.stream.total_in;
      zi->ci.stream.total_in = 0;
    }

    zi->ci.pos_in_buffered_data = 0;

    return err;
}

extern int ZEXPORT zipWriteInFileInZip(zipFile file, const void* buf, unsigned int len)
{
    zip64_internal* zi;
    int err = ZIP_OK;

    if (file == NULL) {
        return ZIP_PARAMERROR;
    }
    zi = (zip64_internal*)file;

    if (zi->in_opened_file_inzip == 0) {
        return ZIP_PARAMERROR;
    }

    zi->ci.crc32 = crc32(zi->ci.crc32, buf, (uInt)len);

#ifdef HAVE_BZIP2
    if(zi->ci.method == Z_BZIP2ED && (!zi->ci.raw)) {
        zi->ci.bstream.next_in = (void*)buf;
        zi->ci.bstream.avail_in = len;
        err = BZ_RUN_OK;

        while ((err == BZ_RUN_OK) && (zi->ci.bstream.avail_in > 0)) {
            if (zi->ci.bstream.avail_out == 0) {
                if (zip64FlushWriteBuffer(zi) == ZIP_ERRNO) {
                    err = ZIP_ERRNO;
                }
                zi->ci.bstream.avail_out = (uInt)Z_BUFSIZE;
                zi->ci.bstream.next_out = (char*)zi->ci.buffered_data;
            }


            if(err != BZ_RUN_OK) {
                break;
            }

            if ((zi->ci.method == Z_BZIP2ED) && (!zi->ci.raw)) {
                uLong uTotalOutBefore_lo = zi->ci.bstream.total_out_lo32;
                //          uLong uTotalOutBefore_hi = zi->ci.bstream.total_out_hi32;
                err = BZ2_bzCompress(&zi->ci.bstream,  BZ_RUN);

                zi->ci.pos_in_buffered_data += (uInt)(zi->ci.bstream.total_out_lo32 - uTotalOutBefore_lo) ;
            }
        }

        if(err == BZ_RUN_OK) {
            err = ZIP_OK;
        }
    } else
#endif
    {
        zi->ci.stream.next_in = (Bytef*)buf;
        zi->ci.stream.avail_in = len;

        while ((err == ZIP_OK) && (zi->ci.stream.avail_in > 0)) {
            if (zi->ci.stream.avail_out == 0) {
                if (zip64FlushWriteBuffer(zi) == ZIP_ERRNO) {
                    err = ZIP_ERRNO;
                }
                zi->ci.stream.avail_out = (uInt)Z_BUFSIZE;
                zi->ci.stream.next_out = zi->ci.buffered_data;
            }


            if(err != ZIP_OK) {
                break;
            }

            if ((zi->ci.method == Z_DEFLATED) && (!zi->ci.raw)) {
                uLong uTotalOutBefore = zi->ci.stream.total_out;
                err=deflate(&zi->ci.stream,  Z_NO_FLUSH);
                if(uTotalOutBefore > zi->ci.stream.total_out) {
                    int bBreak = 0;
                    bBreak++;
                }

                zi->ci.pos_in_buffered_data += (uInt)(zi->ci.stream.total_out - uTotalOutBefore) ;

            } else {

                uInt copy_this;
                if (zi->ci.stream.avail_in < zi->ci.stream.avail_out) {
                    copy_this = zi->ci.stream.avail_in;
                } else {
                    copy_this = zi->ci.stream.avail_out;
                }

                for (uInt i = 0; i < copy_this; i++) {
                    *(((char*)zi->ci.stream.next_out) + i) = *(((const char*)zi->ci.stream.next_in) + i);
                }

                /* Apply to stream */
                {
                    zi->ci.stream.avail_in -= copy_this;
                    zi->ci.stream.avail_out -= copy_this;
                    zi->ci.stream.next_in += copy_this;
                    zi->ci.stream.next_out += copy_this;
                    zi->ci.stream.total_in += copy_this;
                    zi->ci.stream.total_out += copy_this;
                    zi->ci.pos_in_buffered_data += copy_this;
                }
            }
        } // while(...)
    }
    
    return err;
}

extern int ZEXPORT zipCloseFileInZipRaw(zipFile file, uLong uncompressed_size, uLong crc32)
{
    return zipCloseFileInZipRaw64(file, uncompressed_size, crc32);
}

extern int ZEXPORT zipCloseFileInZipRaw64(zipFile file, ZPOS64_T uncompressed_size, uLong crc32)
{
    zip64_internal* zi;
    ZPOS64_T compressed_size;
    uLong invalidValue = 0xffffffff;
    short datasize = 0;
    int err = ZIP_OK;

    if (file == NULL) {
        return ZIP_PARAMERROR;
    }
    zi = (zip64_internal*)file;

    if (zi->in_opened_file_inzip == 0) {
        return ZIP_PARAMERROR;
    }
    zi->ci.stream.avail_in = 0;

    if ((zi->ci.method == Z_DEFLATED) && (!zi->ci.raw)) {
        while (err == ZIP_OK) {
            uLong uTotalOutBefore;
            if (zi->ci.stream.avail_out == 0) {
                if (zip64FlushWriteBuffer(zi) == ZIP_ERRNO) {
                    err = ZIP_ERRNO;
                }
                zi->ci.stream.avail_out = (uInt)Z_BUFSIZE;
                zi->ci.stream.next_out = zi->ci.buffered_data;
            }
            uTotalOutBefore = zi->ci.stream.total_out;
            if (err == ZIP_OK) {
                err = deflate(&zi->ci.stream,  Z_FINISH);
            }
            zi->ci.pos_in_buffered_data += (uInt)(zi->ci.stream.total_out - uTotalOutBefore) ;
        }
    }
#ifdef HAVE_BZIP2
    else if ((zi->ci.method == Z_BZIP2ED) && (!zi->ci.raw)) {
        err = BZ_FINISH_OK;
        while (err == BZ_FINISH_OK) {
            uLong uTotalOutBefore;
            if (zi->ci.bstream.avail_out == 0) {
                if (zip64FlushWriteBuffer(zi) == ZIP_ERRNO) {
                    err = ZIP_ERRNO;
                }
                zi->ci.bstream.avail_out = (uInt)Z_BUFSIZE;
                zi->ci.bstream.next_out = (char*)zi->ci.buffered_data;
            }
            uTotalOutBefore = zi->ci.bstream.total_out_lo32;
            err = BZ2_bzCompress(&zi->ci.bstream,  BZ_FINISH);
            if(err == BZ_STREAM_END) {
                err = Z_STREAM_END;
            }

            zi->ci.pos_in_buffered_data += (uInt)(zi->ci.bstream.total_out_lo32 - uTotalOutBefore);
        }
        
        if(err == BZ_FINISH_OK) {
            err = ZIP_OK;
        }
    }
#endif // HAVE_BZIP2

    if (err == Z_STREAM_END) {
        err = ZIP_OK; /* this is normal */
    }

    if ((zi->ci.pos_in_buffered_data>0) && (err == ZIP_OK)) {
        if (zip64FlushWriteBuffer(zi) == ZIP_ERRNO) {
            err = ZIP_ERRNO;
        }
    }

    if ((zi->ci.method == Z_DEFLATED) && (!zi->ci.raw)) {
        int tmp_err = deflateEnd(&zi->ci.stream);
        if (err == ZIP_OK) {
            err = tmp_err;
        }
        zi->ci.stream_initialised = 0;
    }
#ifdef HAVE_BZIP2
    else if((zi->ci.method == Z_BZIP2ED) && (!zi->ci.raw)) {
      int tmperr = BZ2_bzCompressEnd(&zi->ci.bstream);
        if (err == ZIP_OK) {
            err = tmperr;
        }
        zi->ci.stream_initialised = 0;
    }
#endif

    if (!zi->ci.raw) {
        crc32 = (uLong)zi->ci.crc32;
        uncompressed_size = zi->ci.totalUncompressedData;
    }
    compressed_size = zi->ci.totalCompressedData;

#ifndef NOCRYPT
    compressed_size += (ZPOS64_T)zi->ci.crypt_header_size;
#endif

    // update Current Item crc and sizes,
    if(compressed_size >= 0xffffffff || uncompressed_size >= 0xffffffff || zi->ci.pos_local_header >= 0xffffffff) {
      /*version Made by*/
      zip64local_putValue_inmemory(zi->ci.central_header + 4, (uLong)45, 2);
      /*version needed*/
      zip64local_putValue_inmemory(zi->ci.central_header + 6, (uLong)45, 2);

    }

    zip64local_putValue_inmemory(zi->ci.central_header + 16, crc32, 4); /*crc*/

    if(compressed_size >= 0xffffffff) {
      zip64local_putValue_inmemory(zi->ci.central_header + 20, invalidValue, 4); /*compr size*/
    } else {
      zip64local_putValue_inmemory(zi->ci.central_header + 20, compressed_size, 4); /*compr size*/
    }

    /// set internal file attributes field
    if (zi->ci.stream.data_type == Z_ASCII) {
        zip64local_putValue_inmemory(zi->ci.central_header + 36, (uLong)Z_ASCII, 2);
    }

    if(uncompressed_size >= 0xffffffff) {
      zip64local_putValue_inmemory(zi->ci.central_header + 24, invalidValue, 4); /*uncompr size*/
    } else {
      zip64local_putValue_inmemory(zi->ci.central_header + 24, uncompressed_size, 4); /*uncompr size*/
    }

    // Add ZIP64 extra info field for uncompressed size
    if(uncompressed_size >= 0xffffffff) {
      datasize += 8;
    }

    // Add ZIP64 extra info field for compressed size
    if(compressed_size >= 0xffffffff) {
      datasize += 8;
    }

    // Add ZIP64 extra info field for relative offset to local file header of current file
    if(zi->ci.pos_local_header >= 0xffffffff) {
      datasize += 8;
    }

    if(datasize > 0) {
        char* p = NULL;

        if((uLong)(datasize + 4) > zi->ci.size_centralExtraFree) {
            // we can not write more data to the buffer that we have room for.
            return ZIP_BADZIPFILE;
        }

        p = zi->ci.central_header + zi->ci.size_centralheader;

        // Add Extra Information Header for 'ZIP64 information'
        zip64local_putValue_inmemory(p, 0x0001, 2); // HeaderID
        p += 2;
        zip64local_putValue_inmemory(p, (unsigned short)datasize, 2); // DataSize
        p += 2;

        if(uncompressed_size >= 0xffffffff) {
            zip64local_putValue_inmemory(p, uncompressed_size, 8);
            p += 8;
        }

        if(compressed_size >= 0xffffffff) {
            zip64local_putValue_inmemory(p, compressed_size, 8);
            p += 8;
        }

        if(zi->ci.pos_local_header >= 0xffffffff) {
            zip64local_putValue_inmemory(p, zi->ci.pos_local_header, 8);
            p += 8;
            (void)p;
        }

        // Update how much extra free space we got in the memory buffer
        // and increase the centralheader size so the new ZIP64 fields are included
        // ( 4 below is the size of HeaderID and DataSize field )
        zi->ci.size_centralExtraFree -= (uLong)datasize + 4;
        zi->ci.size_centralheader += (uLong)datasize + 4;

        // Update the extra info size field
        zi->ci.size_centralExtra += (uLong)datasize + 4;
        zip64local_putValue_inmemory(zi->ci.central_header+30,(uLong)zi->ci.size_centralExtra,2);
    }

    if (err == ZIP_OK) {
        err = add_data_in_datablock(&zi->central_dir, zi->ci.central_header, (uLong)zi->ci.size_centralheader);
    }

    free(zi->ci.central_header);

    if (err == ZIP_OK) {
        // Update the LocalFileHeader with the new values.

        ZPOS64_T cur_pos_inzip = (ZPOS64_T)ftello(zi->filestream);

        if (fseeko(zi->filestream, (off_t)(zi->ci.pos_local_header + 14), SEEK_SET) != 0) {
            err = ZIP_ERRNO;
        }

        if (err == ZIP_OK) {
            err = zip64local_putValue(zi->filestream, crc32, 4); /* crc 32, unknown */
        }

        if(uncompressed_size >= 0xffffffff) {
            if(zi->ci.pos_zip64extrainfo > 0) {
                // Update the size in the ZIP64 extended field.
                if (fseeko(zi->filestream, (off_t)(zi->ci.pos_zip64extrainfo + 4), SEEK_SET) != 0) {
                    err = ZIP_ERRNO;
                }

                if (err == ZIP_OK) {
                    /* compressed size, unknown */
                    err = zip64local_putValue(zi->filestream, uncompressed_size, 8);
                }

                if (err == ZIP_OK) {
                    /* uncompressed size, unknown */
                    err = zip64local_putValue(zi->filestream, compressed_size, 8);
                }
            }

        } else {

            if (err == ZIP_OK) {
                /* compressed size, unknown */
                err = zip64local_putValue(zi->filestream, compressed_size, 4);
            }

            if (err == ZIP_OK) {
                /* uncompressed size, unknown */
                err = zip64local_putValue(zi->filestream, uncompressed_size, 4);
            }
        }

        if (fseeko(zi->filestream, (off_t)cur_pos_inzip, SEEK_SET) != 0) {
            err = ZIP_ERRNO;
        }
    }

    zi->number_entry ++;
    zi->in_opened_file_inzip = 0;

    return err;
}

extern int ZEXPORT zipCloseFileInZip(zipFile file)
{
    return zipCloseFileInZipRaw(file, 0, 0);
}

int Write_Zip64EndOfCentralDirectoryLocator(zip64_internal* zi, ZPOS64_T zip64eocd_pos_inzip);
int Write_Zip64EndOfCentralDirectoryLocator(zip64_internal* zi, ZPOS64_T zip64eocd_pos_inzip)
{
    int err = ZIP_OK;
    ZPOS64_T pos = zip64eocd_pos_inzip - zi->add_position_when_writting_offset;

    err = zip64local_putValue(zi->filestream, (uLong)ZIP64ENDLOCHEADERMAGIC, 4);

    /*num disks*/
    if (err == ZIP_OK) {
        /* number of the disk with the start of the central directory */
        err = zip64local_putValue(zi->filestream, (uLong)0, 4);
    }

    /*relative offset*/
    if (err == ZIP_OK) {
        /* Relative offset to the Zip64EndOfCentralDirectory */
        err = zip64local_putValue(zi->filestream, pos, 8);
    }

    /*total disks*/ /* Do not support spawning of disk so always say 1 here*/
    if (err == ZIP_OK) {
        /* number of the disk with the start of the central directory */
        err = zip64local_putValue(zi->filestream, (uLong)1, 4);
    }
    
    return err;
}

int Write_Zip64EndOfCentralDirectoryRecord(zip64_internal* zi, uLong size_centraldir, ZPOS64_T centraldir_pos_inzip);
int Write_Zip64EndOfCentralDirectoryRecord(zip64_internal* zi, uLong size_centraldir, ZPOS64_T centraldir_pos_inzip)
{
    int err = ZIP_OK;

    uLong Zip64DataSize = 44;

    err = zip64local_putValue(zi->filestream, (uLong)ZIP64ENDHEADERMAGIC, 4);

    if (err == ZIP_OK) {
        /* size of this 'zip64 end of central directory' */
        err = zip64local_putValue(zi->filestream, (ZPOS64_T)Zip64DataSize, 8); // why ZPOS64_T of this ?
    }

    if (err == ZIP_OK) {
        /* version made by */
        err = zip64local_putValue(zi->filestream, (uLong)45, 2);
    }

    if (err == ZIP_OK) {
        /* version needed */
        err = zip64local_putValue(zi->filestream, (uLong)45, 2);
    }

    if (err == ZIP_OK) {
        /* number of this disk */
        err = zip64local_putValue(zi->filestream, (uLong)0, 4);
    }

    if (err == ZIP_OK) {
        /* number of the disk with the start of the central directory */
        err = zip64local_putValue(zi->filestream, (uLong)0, 4);
    }

    if (err == ZIP_OK) {
        /* total number of entries in the central dir on this disk */
        err = zip64local_putValue(zi->filestream, zi->number_entry, 8);
    }

    if (err == ZIP_OK) {
        /* total number of entries in the central dir */
        err = zip64local_putValue(zi->filestream, zi->number_entry, 8);
    }

    if (err == ZIP_OK) {
        /* size of the central directory */
        err = zip64local_putValue(zi->filestream, (ZPOS64_T)size_centraldir, 8);
    }

    if (err == ZIP_OK) {
        /* offset of start of central directory with respect to the starting disk number */
        ZPOS64_T pos = centraldir_pos_inzip - zi->add_position_when_writting_offset;
        err = zip64local_putValue(zi->filestream, (ZPOS64_T)pos,8);
    }

    return err;
}

int Write_EndOfCentralDirectoryRecord(zip64_internal* zi, uLong size_centraldir, ZPOS64_T centraldir_pos_inzip);
int Write_EndOfCentralDirectoryRecord(zip64_internal* zi, uLong size_centraldir, ZPOS64_T centraldir_pos_inzip)
{
    int err = ZIP_OK;

    /*signature*/
    err = zip64local_putValue(zi->filestream, (uLong)ENDHEADERMAGIC, 4);

    if (err == ZIP_OK) {
        /* number of this disk */
        err = zip64local_putValue(zi->filestream, (uLong)0, 2);
    }

    if (err == ZIP_OK) {
        /* number of the disk with the start of the central directory */
        err = zip64local_putValue(zi->filestream, (uLong)0, 2);
    }

    if (err == ZIP_OK) {
        /* total number of entries in the central dir on this disk */
        if (zi->number_entry >= 0xFFFF) {
            err = zip64local_putValue(zi->filestream, (uLong)0xffff, 2); // use value in ZIP64 record
        } else {
            err = zip64local_putValue(zi->filestream, (uLong)zi->number_entry, 2);
        }
    }

    if (err == ZIP_OK) {
        /* total number of entries in the central dir */
        if (zi->number_entry >= 0xFFFF) {
            err = zip64local_putValue(zi->filestream, (uLong)0xffff, 2); // use value in ZIP64 record
        } else {
            err = zip64local_putValue(zi->filestream, (uLong)zi->number_entry, 2);
        }
    }

    if (err == ZIP_OK) {
        /* size of the central directory */
        err = zip64local_putValue(zi->filestream, (uLong)size_centraldir, 4);
    }

    if (err == ZIP_OK) {
        /* offset of start of central directory with respect to the starting disk number */
        ZPOS64_T pos = centraldir_pos_inzip - zi->add_position_when_writting_offset;
        if(pos >= 0xffffffff) {
            err = zip64local_putValue(zi->filestream, (uLong)0xffffffff, 4);
        } else {
            err = zip64local_putValue(zi->filestream, (uLong)(centraldir_pos_inzip - zi->add_position_when_writting_offset), 4);
        }
    }
    
    return err;
}

int Write_GlobalComment(zip64_internal* zi, const char* global_comment);
int Write_GlobalComment(zip64_internal* zi, const char* global_comment)
{
    int err = ZIP_OK;
    uInt size_global_comment = 0;

    if(global_comment != NULL) {
        size_global_comment = (uInt)strlen(global_comment);
    }

    err = zip64local_putValue(zi->filestream, (uLong)size_global_comment, 2);

    if (err == ZIP_OK && size_global_comment > 0) {
        if (fwrite(global_comment, 1, size_global_comment, zi->filestream) != size_global_comment) {
            err = ZIP_ERRNO;
        }
    }
    return err;
}

extern int ZEXPORT zipClose(zipFile file, const char* global_comment)
{
    zip64_internal* zi;
    int err = 0;
    uLong size_centraldir = 0;
    ZPOS64_T centraldir_pos_inzip;
    ZPOS64_T pos;

    if (file == NULL) {
        return ZIP_PARAMERROR;
    }

    zi = (zip64_internal*)file;

    if (zi->in_opened_file_inzip == 1) {
        err = zipCloseFileInZip (file);
    }

#ifndef NO_ADDFILEINEXISTINGZIP
    if (global_comment == NULL) {
        global_comment = zi->globalcomment;
    }
#endif

    centraldir_pos_inzip = (ZPOS64_T)ftello(zi->filestream);

    if (err == ZIP_OK) {
        linkedlist_datablock_internal* ldi = zi->central_dir.first_block;
        while (ldi != NULL) {
            if ((err == ZIP_OK) && (ldi->filled_in_this_block > 0)) {
                if (fwrite(ldi->data, 1, ldi->filled_in_this_block, zi->filestream)  != ldi->filled_in_this_block) {
                    err = ZIP_ERRNO;
                }
            }

            size_centraldir += ldi->filled_in_this_block;
            ldi = ldi->next_datablock;
        }
    }
    free_linkedlist(&(zi->central_dir));

    pos = centraldir_pos_inzip - zi->add_position_when_writting_offset;
    if(pos >= 0xffffffff) {
        ZPOS64_T Zip64EOCDpos = (ZPOS64_T)ftello(zi->filestream);
        Write_Zip64EndOfCentralDirectoryRecord(zi, size_centraldir, centraldir_pos_inzip);

        Write_Zip64EndOfCentralDirectoryLocator(zi, Zip64EOCDpos);
    }

    if (err == ZIP_OK) {
        err = Write_EndOfCentralDirectoryRecord(zi, size_centraldir, centraldir_pos_inzip);
    }

    if(err == ZIP_OK) {
        err = Write_GlobalComment(zi, global_comment);
    }

    if (fclose(zi->filestream) != 0) {
        if (err == ZIP_OK) {
            err = ZIP_ERRNO;
        }
    }

#ifndef NO_ADDFILEINEXISTINGZIP
    TRYFREE(zi->globalcomment);
#endif
    TRYFREE(zi);
    
    return err;
}

extern int ZEXPORT zipRemoveExtraInfoBlock (char* pData, int* dataLen, short sHeader)
{
    char* p = pData;
    int size = 0;
    char* pNewHeader;
    char* pTmp;
    short header;
    short dataSize;

    int retVal = ZIP_OK;

    if(pData == NULL || *dataLen < 4) {
        return ZIP_PARAMERROR;
    }

    pNewHeader = (char*)ALLOC((unsigned int)*dataLen);
    pTmp = pNewHeader;

    while(p < (pData + *dataLen)) {
        header = *(short*)p;
        dataSize = *(((short*)p)+1);

        if( header == sHeader ) {
            // Header found.
            p += dataSize + 4; // skip it. do not copy to temp buffer
        } else {
            // Extra Info block should not be removed, So copy it to the temp buffer.
            memcpy(pTmp, p, dataSize + 4);
            p += dataSize + 4;
            size += dataSize + 4;
        }
    }

    if(size < *dataLen) {
        // clean old extra info block.
        memset(pData,0, *dataLen);

        // copy the new extra info block over the old
        if(size > 0) {
            memcpy(pData, pNewHeader, size);
        }
        
        // set the new extra info size
        *dataLen = size;
        
        retVal = ZIP_OK;

    } else {
        retVal = ZIP_ERRNO;
    }
    
    TRYFREE(pNewHeader);
    
    return retVal;
}
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

// reference: github Inori/FuckGalEngine
// arguments: prog x|c infile|infolder outfile|outfolder [-xor]

typedef struct IGA_header {
    char signature[4]; // "IGA0"
    uint32_t unknown1; // seems like a checksum
    uint32_t unknown2; // 02 00 00 00 (LE:2)
    uint32_t unknown3; // 02 00 00 00 (LE:2)
} IGA_header;

int iga_xtract(const char *infile, const char *outfolder, bool xor);
int iga_create(const char *infolder, const char *outfile, bool xor);

int main(int argc, char **argv) {
    bool xor_flag = false;
    if (argc < 4 || argc > 5) {
        fprintf(stderr, "usage: program x|c infile|infolder outfile|outfolder [-xor]\n");
        fprintf(stderr, " note: if you get a segfault, your iga file may be corrupt.\n");
        fprintf(stderr, " note:   we presume that end_filenames equals to data_base.\n");
        return -1;
    }
    if (argc == 5) {
        if (strcmp(argv[4], "-xor") == 0)
            xor_flag = true;
        else {
            fprintf(stderr, "err: mistyped \"-xor\"?\n");
            return -1;
        }
    }
    if (strcmp(argv[1], "x") == 0)
        return iga_xtract(argv[2], argv[3], xor_flag);
    if (strcmp(argv[1], "c") == 0)
        return iga_create(argv[2], argv[3], xor_flag);
    fprintf(stderr, "err: unknown operation.\n");
    return -1;
}

// return new pointer
unsigned char *get_multibyte_long(unsigned char *ptr, unsigned long *val) {
    unsigned long v = 0;
    while (!(v & 1)) // LSB marks whether we have more bytes
        v = (v << 7) | *(ptr++);
    *val = v >> 1;
    return ptr;
}

int iga_xtract(const char *infile, const char *outfolder, bool xor) {
    int fd = open(infile, O_RDONLY);
    if (fd == -1) {
        perror("err: cannot open infile");
        return -1;
    }
    size_t file_len;
    {
        struct stat buf;
        if (fstat(fd, &buf) == -1) {
            perror("err: cannot fstat infile");
            close(fd);
            return -1;
        }
        file_len = buf.st_size;
    }
    uint8_t *file_map = mmap(NULL, file_len, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (file_map == MAP_FAILED) {
        perror("err: cannot mmap infile");
        return -1;
    }
    bool success = true;
    IGA_header *iga_hdr = (IGA_header *)file_map;
    do {
        if (strncmp(iga_hdr->signature, "IGA0", 4) != 0) {
            fprintf(stderr, "err: infile has wrong signature.\n");
            success = false;
            break;
        }
        int addslash = 0;     // will also be added to final path length
        uint8_t xorvalue = 0; // will xor with data: 0 is original, 0xFF is xor
        size_t len_foldername = strlen(outfolder);
        if (outfolder[len_foldername - 1] != '/')
            addslash = 1;
        if (xor)
            xorvalue = 0xFF;

        uint8_t *ptr_entries = file_map + sizeof(IGA_header), *ptr_filenames;
        unsigned long len_entries, len_filenames;
        ptr_entries = get_multibyte_long(ptr_entries, &len_entries); // = 'entries' block begin
        uint8_t *end_entries = ptr_entries + len_entries;
        ptr_filenames = get_multibyte_long(end_entries, &len_filenames); // = 'filenames' block begin
        uint8_t *end_filenames = ptr_filenames + len_filenames, *data_base = end_filenames;
        printf("info: entries_len = %lu\n", len_entries);
        printf("info: filenms_len = %lu\n", len_filenames);
        bool is_first = true;
        unsigned long filename_offset = 0, offset = 0, length = 0, entry_count = 0;
        while (ptr_entries < end_entries) {
            unsigned long nfilename_offset, noffset, nlength;
            ptr_entries = get_multibyte_long(ptr_entries, &nfilename_offset);
            ptr_entries = get_multibyte_long(ptr_entries, &noffset);
            ptr_entries = get_multibyte_long(ptr_entries, &nlength);
            if (!is_first) // actually we are processing the previous entry
            // COPY begin
            {
                unsigned long ch;
                size_t filename_length = nfilename_offset - filename_offset, j = 0;
                char filename[filename_length + 1];
                char outpath[len_foldername + addslash + filename_length + 1];
                for (size_t i = 0; j < len_foldername; i++, j++)
                    outpath[j] = outfolder[i];
                if (addslash)
                    outpath[j++] = '/';
                for (size_t i = 0; i < filename_length; i++)
                    ptr_filenames = get_multibyte_long(ptr_filenames, &ch),
                    filename[i] = (char)ch,
                    outpath[j++] = (char)ch;
                outpath[j] = '\0';
                filename[filename_length] = '\0';
                int outfd = open(outpath, O_RDWR | O_CREAT), err;
                if (outfd == -1) {
                    perror("err: cannot open output file for write");
                    success = false;
                    goto jumpout;
                }
                fchmod(outfd, 0644); // we dont need to ensure a success chmod
                if ((err = posix_fallocate(outfd, 0, length)) != 0) {
                    fprintf(stderr, "err: cannot fallocate space for file: %s\n", strerror(err));
                    success = false;
                    close(outfd);
                    goto jumpout;
                }
                uint8_t *buffer = malloc(length),
                        *pData = buffer, *pOriginal = data_base + offset;
                if (buffer == NULL) {
                    perror("err: cannot malloc memory for decrypted file data");
                    success = false;
                    close(outfd);
                    goto jumpout;
                }
                for (size_t i = 0; i < length; i++)
                    *(pData++) = *(pOriginal++) ^ ((unsigned char)i + 2) ^ xorvalue;
                if ((err = write(outfd, buffer, length)) != length) {
                    if (err == -1) {
                        perror("err: when writing file");
                        success = false;
                        close(outfd);
                        free(buffer);
                        goto jumpout;
                    }
                }
                close(outfd);
                free(buffer);
                entry_count++;
                printf("entry: (#%3ld) filename = \"%s\" offset = %ld length = %ld\n", entry_count, filename, offset, length);
                // printf("debug: fullpath = \"%s\"\n", outpath);
            }
            // COPY end
            is_first = false;
            filename_offset = nfilename_offset;
            offset = noffset;
            length = nlength;
        }
        if (is_first) {
            fprintf(stderr, "err: no entry in this iga file?\n");
            success = false;
            break;
        }
        // PASTE begin
        {
            unsigned long ch;
            size_t filename_length = len_filenames - filename_offset, j = 0;
            char filename[filename_length + 1];
            char outpath[len_foldername + addslash + filename_length + 1];
            for (size_t i = 0; j < len_foldername; i++, j++)
                outpath[j] = outfolder[i];
            if (addslash)
                outpath[j++] = '/';
            for (size_t i = 0; i < filename_length; i++)
                ptr_filenames = get_multibyte_long(ptr_filenames, &ch),
                filename[i] = (char)ch,
                outpath[j++] = (char)ch;
            outpath[j] = '\0';
            filename[filename_length] = '\0';
            int outfd = open(outpath, O_RDWR | O_CREAT), err;
            if (outfd == -1) {
                perror("err: cannot open output file for write");
                success = false;
                goto jumpout;
            }
            fchmod(outfd, 0644); // we dont need to ensure a success chmod
            if ((err = posix_fallocate(outfd, 0, length)) != 0) {
                fprintf(stderr, "err: cannot fallocate space for file: %s\n", strerror(err));
                success = false;
                close(outfd);
                goto jumpout;
            }
            uint8_t *buffer = malloc(length),
                    *pData = buffer, *pOriginal = data_base + offset;
            if (buffer == NULL) {
                perror("err: cannot malloc memory for decrypted file data");
                success = false;
                close(outfd);
                goto jumpout;
            }
            for (size_t i = 0; i < length; i++)
                *(pData++) = *(pOriginal++) ^ ((unsigned char)i + 2) ^ xorvalue;
            if ((err = write(outfd, buffer, length)) != length) {
                if (err == -1) {
                    perror("err: when writing file");
                    success = false;
                    close(outfd);
                    free(buffer);
                    goto jumpout;
                }
            }
            close(outfd);
            free(buffer);
            entry_count++;
            printf("entry: (#%3ld) filename = \"%s\" offset = %ld length = %ld\n", entry_count, filename, offset, length);
            // printf("debug: fullpath = \"%s\"\n", outpath);
        }
        // PASTE end
        printf("debug: ptr_filenames %s end_filenames\n", ptr_filenames == end_filenames ? "==" : "!=");
        printf("debug: end_filenames %s data_base (in the one from FuckGalEngine)\n",
               end_filenames == (file_map + file_len - offset - length) ? "==" : "!=");
        printf("debug: xor = %s, xorvalue = 0x%X\n", xor? "true" : "false", xorvalue);
    } while (false);
jumpout:
    munmap(file_map, file_len);
    return success ? 0 : -1;
}
int iga_create(const char *infolder, const char *outfile, bool xor) {
    printf("dummy function.\n");
    return 0;
}
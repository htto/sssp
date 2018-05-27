/**
 * Read a stean stats schema file
 *   ./exe UserGameStatsSchema_[0-9]+.bin
 * Compile with
 *   gcc -o exe statsread.c -Wall -Wextra
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define iprint(indent, args...) \
{ \
    for (size_t i = 0; i < indent; i++) { \
        fprintf(stderr, " "); \
    } \
    fprintf(stderr, args); \
}

size_t dump(const char *data, size_t size, size_t offset, size_t indent)
{
    u_int8_t type;
    char key[1024];
    char value[2048];
    size_t keylen = 0;
    size_t vallen = 0;
    size_t off = offset;
    u_int16_t len;


    while (off < size) {
        type = *(data + off++);

        if (type == 8) {
            break;
        }

        keylen = strlen(data + off) + 1;
        strncpy(key, data + off, keylen);
        key[keylen - 1] = 0;
        off += keylen;

        if (type == 0) {
            iprint(indent, "key: %s, value: [\n", key);
        }

        switch (type) {
            case 0:
                //iprint(indent, "SUB");
                off += dump(data, size, off, indent + 2);
                break;
            case 1:
                //iprint(indent, "STRING %s ", data + off);
                vallen = strlen(data + off) + 1;
                strncpy(value, data + off, vallen);
                value[vallen - 1] = 0;
                off += vallen;
                break;
            case 2:
                //iprint(indent, "INT");
                //iprint(indent, " %d ", *((int32_t*)(data + off)));

                snprintf(value, 20, "%d", *((int32_t*)(data + off)));
                off += 4;
                break;
            case 3:
                off += 4;
                //iprint(indent, "FLOAT");
                break;
            case 4:
                //iprint(indent, "PTR");
                off += 4;
                break;
            case 5:
                //iprint(indent, "WSTRING");
                len = *(data + off) << 8 | *(data + off + 1);
                //iprint(indent, "len: %u", len);
                off += 2;
                off += sizeof(wchar_t) * len;
                break;
            case 6:
                //iprint(indent, "COLOR");
                off += 4; // 4 char
                break;
            case 7:
                //iprint(indent, "UINT64");
                off += 8;
                break;
        }

        if (type != 0) {
            iprint(indent, "key: %s, value: %s\n", key, value);
        } else {
            iprint(indent, "]\n");
        }
    }

    return off - offset;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        return 1;
    }

    char *data;
    const char *file = argv[1];
    size_t size;

    FILE *fp = fopen(file, "r");
    if (fp) {
        fseek(fp, 0, SEEK_END);
        size = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        data = malloc(size);
        fread(data, size, 1, fp);
        dump(data, size, 0, 0);
        free(data);
        fclose(fp);
    }

    return 0;
}

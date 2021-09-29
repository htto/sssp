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

#define KEY_MAX 128
#define VAL_MAX 2048
#define iprint(indent, args...) \
{ \
    for (size_t i = 0; i < indent; i++) { \
        fprintf(stdout, " "); \
    } \
    fprintf(stdout, args); \
}

enum {
    TYPE_COLLECTION = 0,
    TYPE_STRING,
    TYPE_INTEGER,
    TYPE_FLOAT,
    TYPE_POINTER,
    TYPE_WSTRING,
    TYPE_COLOR,
    TYPE_UNSIGNED_INTEGER,
    TYPE_END
};

typedef struct statsnode_s
{
    u_int8_t type;
    char name[KEY_MAX];
    void *data;
    size_t datalen;
    struct statsnode_s *parent;
    struct statsnode_s **children;
    ssize_t numchildren;
} statsnode_t;

size_t collect(const char *data, size_t size, size_t offset, statsnode_t *node)
{
    u_int8_t type;
    char value[VAL_MAX];
    size_t keylen = 0;
    size_t vallen = 0;
    size_t off = offset;
    u_int16_t len;
    statsnode_t *child;

    while (off < size) {
        // Get type
        type = *(data + off++);

        // EON
        if (type == TYPE_END) {
            break;
        }

        // Add a new node holding the data to our parent
        node->numchildren++;
        node->children = realloc(node->children, node->numchildren * sizeof(node));
        child = node->children[node->numchildren - 1] = calloc(1, sizeof(*node));
        child->parent = node;

        // Set type and name
        child->type = type;
        keylen = strlen(data + off) + 1;
        assert(keylen < KEY_MAX);
        strncpy(child->name, data + off, keylen);
        child->name[keylen - 1] = 0;
        off += keylen;

        // @fixme assert there's enough room in the source buffer
        switch (type) {
            case TYPE_COLLECTION:
                off += collect(data, size, off, child);
                break;
            case TYPE_STRING:
                vallen = strlen(data + off) + 1;
                strncpy(value, data + off, vallen);
                value[vallen - 1] = 0;
                break;
            case TYPE_INTEGER:
                vallen = 4;
                snprintf(value, 20, "%d", *((int32_t*)(data + off)));
                break;
            case TYPE_FLOAT:
                vallen = 4;
                break;
            case TYPE_POINTER:
                vallen = 4;
                break;
            case TYPE_WSTRING:
                len = *(data + off) << 8 | *(data + off + 1);
                vallen = 2 + sizeof(wchar_t) * len;
                break;
            case TYPE_COLOR:
                vallen = 4; // 4 char
                break;
            case TYPE_UNSIGNED_INTEGER:
                vallen = 8;
                break;
        }

        if (type != TYPE_COLLECTION) {
            child->datalen = vallen;
            assert(!child->data);
            child->data = malloc(vallen);
            memcpy(child->data, value, vallen);
        }

        off += vallen;
    }

    return off - offset;
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
    statsnode_t root;
    bzero(&root, sizeof(root));

    FILE *fp = fopen(file, "r");
    if (fp) {
        fseek(fp, 0, SEEK_END);
        size = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        data = malloc(size);
        fread(data, size, 1, fp);
        collect(data, size, 0, &root);
        dump(data, size, 0, 0);
        free(data);
        fclose(fp);
    }

    return 0;
}

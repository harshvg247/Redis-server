#ifndef PARSER_H
#define PARSER_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "utils.h" // For count_digits()

#define dbg 1 // For debug prints

// --- Request Parsing ---

static inline int extract_number(int *ind, const char *str)
{
    int num = 0;
    int minus = 1;
    if(str[*ind] == '-'){
        minus = -1;
        (*ind)++;
    }
    while (str[*ind] - '0' >= 0 && str[*ind] - '0' <= 9)
    {
        num = num * 10 + (str[(*ind)++] - '0');
    }
    return minus*num;
}

static inline char *extract_bulk_string(int *ind, const char *str)
{
    if (dbg)
        printf("Extracting bulk string\n");
    (*ind)++;
    int bulk_str_size = extract_number(ind, str);
    if (dbg)
        printf("bulk_str_size: %d\n", bulk_str_size);
    (*ind) += 2;
    char *bulk_str = (char *)malloc(bulk_str_size + 1);
    if (!bulk_str) return NULL; // Malloc check

    for (int i = 0; i < bulk_str_size; i++)
    {
        bulk_str[i] = str[(*ind)++];
    }
    bulk_str[bulk_str_size] = '\0';
    if (dbg)
        printf("Extracted string: %s\n", bulk_str);
    return bulk_str;
}

// --- Response Encoding ---

static inline char *encode_bulk_str(const char *str)
{
    int str_len = strlen(str);
    int num_digits_in_size_str = count_digits(str_len);
    
    // $ + digits + \r\n + data + \r\n + \0
    char *encoded_str = (char *)malloc(1 + num_digits_in_size_str + 2 + str_len + 2 + 1);
    if (encoded_str == NULL)
    {
        return NULL; // Handle allocation failure
    }
    sprintf(encoded_str, "$%d\r\n%s\r\n", str_len, str);
    return encoded_str;
}

static inline char *encode_integer(long long val)
{
    // :<val>\r\n\0
    // A 64-bit integer is at most 20 digits + sign + 3 chars + null
    char *encoded_str = (char *)malloc(25);
    if (encoded_str == NULL)
        return NULL;
    sprintf(encoded_str, ":%lld\r\n", val);
    return encoded_str;
}

#endif // PARSER_H
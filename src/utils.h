#ifndef UTILS_H
#define UTILS_H

#include <ctype.h>

int count_digits(int n)
{
    if (n == 0)
        return 1;

    int count = 0;
    if (n < 0)
        n = -n; // ignore sign

    while (n > 0)
    {
        n /= 10;
        count++;
    }

    return count;
}
void to_lowercase(char *str)
{
    for (int i = 0; str[i] != '\0'; i++)
    {
        str[i] = tolower((unsigned char)str[i]);
    }
}
#endif
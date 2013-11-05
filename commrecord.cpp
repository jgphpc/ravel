#include "commrecord.h"

CommRecord::CommRecord(unsigned int _s, unsigned long long int _st,
                       unsigned int _r, unsigned long long int _rt,
                       unsigned int _size, unsigned int _tag) :
    sender(_s), send_time(_st), receiver(_r), recv_time(_rt),
    size(_size), tag(_tag)
{
    matched = false;
}
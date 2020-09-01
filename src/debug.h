#ifndef __MESH_DEBUG_H
#define __MESH_DEBUG_H

#include <Arduino.h>

//#define DEBUG_MESH

#ifdef DEBUG_MESH
#define DEBUG_WRITE(msg) Serial.print(msg)
#define DEBUG_WRITELN(msg) Serial.println(msg)
#define DEBUG_WRITEHEXVAL(val, len) {           \
    char temp[9];                               \
    sprintf(temp, "%08X", (unsigned int)(val)); \
    Serial.print(&temp[8-len]);                 \
}
#define DEBUG_WRITEHEXVAL64(val) {                  \
    char temp[3];                                   \
    unsigned char *valptr = (unsigned char *)&val;  \
    char lencount = 16;                             \
    valptr += 7;                                    \
    while(lencount) {                               \
        sprintf(temp, "%02X", *valptr);             \
        lencount -= 2;                              \
        valptr--;                                   \
        Serial.print(temp);                         \
    }                                               \
}
#define DEBUG_WRITEMAC(mac) {Serial.printf("%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);}
#define DEBUG_DUMPHEX(msg, data, datalen) {      \
    char *debugdata = (char *)(data);       \
    int debuglen;                           \
    int outlen;                             \
    char temp[40];                          \
    if(msg)                                 \
        Serial.println(msg);                \
    for(debuglen = 0, outlen = 0; debuglen < (datalen); debuglen++, debugdata++) {   \
        if(debuglen && ((debuglen & 0xf) == 0)) {                        \
            temp[outlen] = 0;                                            \
            Serial.println(temp);                                          \
            temp[0] = 0;                                                 \
            outlen = 0;                                                  \
        } else if(debuglen && ((debuglen & 0x7) == 0)) {                 \
            temp[outlen++] = ' ';                                        \
            temp[outlen++] = '-';                                        \
            temp[outlen++] = ' ';                                        \
        } else if(debuglen && ((debuglen & 0x3) == 0))                   \
            temp[outlen++] = ' ';                                        \
        sprintf(&temp[outlen], "%02X", *debugdata);                      \
        outlen += 2;                                                     \
    }                                                                    \
    if(outlen) Serial.println(temp);                                     \
}
#else
#define DEBUG_WRITE(msg) {}
#define DEBUG_WRITELN(msg) {}
#define DEBUG_WRITEHEXVAL(val, len) {}
#define DEBUG_WRITEMAC(mac) {}
#define DEBUG_DUMPHEX(msg, data, datalen) {}
#endif

#endif
/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Portable C implementations of FastDoom's Watcom #pragma-aux math/memory
 * helpers. These preserve the 32-bit fixed-point contracts used by the game
 * and portable renderer.
 */
#include <stdint.h>
#include <string.h>

#include "doomtype.h"
#include "fastmath.h"

static int32_t clamp_i64_to_i32(int64_t v)
{
    if (v > INT32_MAX)
        return INT32_MAX;
    if (v < INT32_MIN)
        return INT32_MIN;
    return (int32_t)v;
}

fixed_t FixedMul(fixed_t a, fixed_t b)
{
    return (fixed_t)(((int64_t)a * (int64_t)b) >> 16);
}

int FixedMulShortToInt(fixed_t a, fixed_t b)
{
    return (int)(((int64_t)a * (int64_t)b) >> 16);
}

fixed_t FixedMulECX(fixed_t a, fixed_t b)
{
    return FixedMul(a, b);
}

fixed_t FixedMulEDX(fixed_t a, fixed_t b)
{
    return FixedMul(a, b);
}

fixed_t FixedMulEDXHalf(fixed_t a, fixed_t b)
{
    return (fixed_t)(((int64_t)a * (int64_t)b) >> 17);
}

fixed_t FixedMulSquare(fixed_t a)
{
    return FixedMul(a, a);
}

fixed_t FixedMulHStep(fixed_t a, fixed_t b)
{
    uint64_t p = (uint64_t)((int64_t)a * (int64_t)b);
    return (fixed_t)((uint32_t)(p >> 6) & 0xffff0000u);
}

fixed_t FixedMulLStep(fixed_t a, fixed_t b)
{
    uint64_t p = (uint64_t)((int64_t)a * (int64_t)b);
    return (fixed_t)((uint32_t)(p >> 22) & 0x0000ffffu);
}

fixed_t FixedDiv2(fixed_t a, fixed_t b)
{
    if (b == 0)
        return (a < 0) ? INT32_MIN : INT32_MAX;
    return clamp_i64_to_i32(((int64_t)a << 16) / (int64_t)b);
}

fixed_t FixedDivDBITS(fixed_t a, fixed_t b)
{
    if (b == 0)
        return (a < 0) ? INT32_MIN : INT32_MAX;
    return clamp_i64_to_i32(((int64_t)a << 11) / (int64_t)b);
}

fixed_t FixedDiv65536(fixed_t b)
{
    if (b == 0)
        return INT32_MAX;
    return clamp_i64_to_i32(((int64_t)1 << 32) / (int64_t)b);
}

unsigned char ROLAND1(int value)
{
    return (unsigned char)(((uint32_t)value) >> 31);
}

int Mul20(int value)      { return value * 20; }
int Mul40(int value)      { return value * 40; }
int Mul80(int value)      { return value * 80; }
int Mul320(int value)     { return value * 320; }
int Mul640(int value)     { return value * 640; }
int Mul1600(int value)    { return value * 1600; }
int Mul1280(int value)    { return value * 1280; }
int Mul64(int value)      { return value * 64; }
int Mul128(int value)     { return value * 128; }
int Mul256(int value)     { return value * 256; }
int Mul512(int value)     { return value * 512; }
int Mul1024(int value)    { return value * 1024; }
int Mul10(int value)      { return value * 10; }
unsigned short USMul10(unsigned short value)
{
    return (unsigned short)(value * 10u);
}
int Mul100(int value)     { return value * 100; }
int Mul200(int value)     { return value * 200; }
int Mul400(int value)     { return value * 400; }
int Mul800(int value)     { return value * 800; }
unsigned short USMul100(unsigned short value)
{
    return (unsigned short)(value * 100u);
}
int Mul1000(int value)    { return value * 1000; }
unsigned short USMul1000(unsigned short value)
{
    return (unsigned short)(value * 1000u);
}
int Mul819200(int value)  { return value * 819200; }
int Mul35(int value)      { return value * 35; }
int Mul175(int value)     { return value * 175; }
int Mul768(int value)     { return value * 768; }
int Mul160(int value)     { return value * 160; }
int Mul409(int value)     { return value * 409; }
int Mul70(int value)      { return value * 70; }
int Mul47000(int value)   { return value * 47000; }

int Div1000(int value)       { return value / 1000; }
int Div10(int value)         { return value / 10; }
int Div3(int value)          { return value / 3; }
int Div63(int value)         { return value / 63; }
int Div101(int value)        { return value / 101; }
int Div35(int value)         { return value / 35; }
int DivSKULLSPEED(int value) { return value / (20 * FRACUNIT); }
int Div100(int value)        { return value / 100; }
int Mul25(int value)         { return value * 25; }
int Mul75(int value)         { return value * 75; }
int Div255(int value)        { return value / 255; }
unsigned long Div51200(unsigned long value)
{
    return value / 51200ul;
}
int Div70(int value)         { return value / 70; }
int Div84(int value)         { return value / 84; }
int Div128(int value)        { return (int)((uint32_t)value >> 7); }

void CopyBytes(void *src, void *dest, int num_bytes)
{
    if (num_bytes > 0)
        memcpy(dest, src, (size_t)num_bytes);
}

void CopyWords(void *src, void *dest, int num_words)
{
    if (num_words > 0)
        memcpy(dest, src, (size_t)num_words * sizeof(uint16_t));
}

void CopyDWords(void *src, void *dest, int num_dwords)
{
    if (num_dwords > 0)
        memcpy(dest, src, (size_t)num_dwords * sizeof(uint32_t));
}

void SetBytes(void *dest, unsigned char value, int num_bytes)
{
    if (num_bytes > 0)
        memset(dest, value, (size_t)num_bytes);
}

void SetWords(void *dest, short value, int num_words)
{
    int i;
    uint16_t *p = (uint16_t *)dest;
    for (i = 0; i < num_words; ++i)
        p[i] = (uint16_t)value;
}

void SetDWords(void *dest, int value, int num_dwords)
{
    int i;
    uint32_t *p = (uint32_t *)dest;
    for (i = 0; i < num_dwords; ++i)
        p[i] = (uint32_t)value;
}

void OutString(unsigned short Port, unsigned char *addr, int c)
{
    (void)Port;
    (void)addr;
    (void)c;
}

void FastPaletteOut(unsigned char *addr)
{
    (void)addr;
}

unsigned char InByte60h(void) { return 0; }
unsigned char InByte61h(void) { return 0; }
void OutByte20h(unsigned char al) { (void)al; }
void OutByteA0h(unsigned char al) { (void)al; }
void OutByte42h(unsigned char al) { (void)al; }
void OutByte43h(unsigned char al) { (void)al; }
void OutByte61h(unsigned char al) { (void)al; }

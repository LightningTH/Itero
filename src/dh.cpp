#include <Arduino.h>
#include "mesh_internal.h"

void MeshNetworkInternal::DHMul64(unsigned long long a, unsigned long long b, unsigned long long *ret)
{
    unsigned long long low;
    unsigned long long mid1;
    unsigned long long mid2;
    unsigned long long high;
    unsigned long long a_part[2];
    unsigned long long b_part[2];

    //split up our 64bit incoming values so we can properly operate on it
    a_part[0] = a & 0xffffffff;
    a_part[1] = a >> 32;
    b_part[0] = b & 0xffffffff;
    b_part[1] = b >> 32;

    //handle portions
    high = (a_part[1] * b_part[1]);
    mid1 = (a_part[1] * b_part[0]);
    mid2 = (a_part[0] * b_part[1]);
    low = (a_part[0] * b_part[0]);

    //combine
    ret[0] = low + ((mid1 << 32)) + ((mid2 << 32));
    ret[1] = high + (mid1 >> 32) + (mid2 >> 32);
    if(((mid1 & 0xffffffff) + (mid2 & 0xffffffff)) >> 32)
        ret[1]++;
}

unsigned long long MeshNetworkInternal::DHMod128(unsigned long long a[2], unsigned long long b)
{
    unsigned long long x[2];
    unsigned long long adiv2[2];

    x[0] = b;
    x[1] = 0;
    adiv2[0] = a[0] >> 1;
    adiv2[1] = a[1] >> 1;
    adiv2[0] |= (a[1] & 1) << 63;

    while(1)
    {
        //if X > (A / 2) break
        if((x[1] > adiv2[1]) || ((x[1] == adiv2[1]) && (x[0] > adiv2[0])))
            break;

        x[1] <<= 1;
        if(x[0] >> 63)
            x[1] |= 1;
        x[0] <<= 1;
    }

    while(1)
    {
        //if A < B break
        if(!a[1] && (a[2] < b))
            break;

        //if A >= X {A -= X}
        if((a[1] > x[1]) || ((a[1] == x[1]) && (a[0] >= x[0])))
        {
            adiv2[0] = a[0] - x[0];
            adiv2[1] = a[1] - x[1];
            if(adiv2[0] < a[0])
                adiv2[1]--;
        }

        //x >>= 1
        x[0] >>= 1;
        x[0] |= (x[1] << 63);
        x[1] >>= 1;
    }

    return a[0];
}

unsigned long long MeshNetworkInternal::DHPowMod(unsigned long long g, unsigned long long pr)
{
    //do a 64bit pow/mod, (g**pr) % m
    unsigned long long result[2];
    unsigned long long gl[2];
    unsigned long long ml;

    gl[0] = g;
    gl[1] = 0;
    ml = this->DH_P;
    result[0] = 1;
    result[1] = 0;
    while(pr)
    {
        //if set then do a multiple and modulus
        if(pr & 1)
        {
            //result = (result * gl) % ml;
            this->DHMul64(result[0], gl[0], result);
            result[0] = this->DHMod128(result, ml);
            result[1] = 0;
        }

        //bit shift
        pr >>= 1;
        //gl = (gl * gl) % ml;
        this->DHMul64(gl[0], gl[0], gl);
        gl[0] = this->DHMod128(gl, ml);
        gl[1] = 0;
    };

    return result[0];
}

//return our value and set the provided value to what the other side is given
unsigned long long MeshNetworkInternal::DHCreateChallenge(unsigned long long *challenge)
{
    //select a random value
    union
    {
        unsigned int priv[2];
        unsigned long long priv64;
    };
    priv[0] = esp_random();
    priv[1] = esp_random();

    *challenge = this->DHPowMod(this->DH_G, priv64);
    DEBUG_WRITE("DHCreateChallenge priv ");
    DEBUG_WRITEHEXVAL64(priv64);
    DEBUG_WRITE(", challenge ");
    DEBUG_WRITEHEXVAL64(*challenge);
    DEBUG_WRITE("\n");
    return priv64;
}

unsigned long long MeshNetworkInternal::DHFinishChallenge(unsigned long long priv, unsigned long long challenge)
{
    unsigned long long ret;

    ret = this->DHPowMod(challenge, priv);
    DEBUG_WRITE("DHFinishCHallenge priv ");
    DEBUG_WRITEHEXVAL64(priv);
    DEBUG_WRITE(", challenge ");
    DEBUG_WRITEHEXVAL64(challenge);
    DEBUG_WRITE(", ret ");
    DEBUG_WRITEHEXVAL64(ret);
    DEBUG_WRITE("\n");

    return ret;

}

int MeshNetworkInternal::DHInit(unsigned long long P, unsigned long long G)
{
    //if G is larger than P then fail
    if(G > P)
        return -1;

    this->DH_P = P;
    this->DH_G = G;

    DEBUG_WRITE("DHInit P ");
    DEBUG_WRITEHEXVAL64(P);
    DEBUG_WRITE(", G ");
    DEBUG_WRITEHEXVAL64(G);
    DEBUG_WRITE("\n");

    return 0;
}
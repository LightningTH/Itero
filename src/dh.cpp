#include <Arduino.h>
#include "mesh_internal.h"

//#define DH_P 4169116887
//#define DH_G 3889611491

unsigned int MeshNetworkInternal::DHPowMod(unsigned int g, unsigned int pr)
{
    //do a 32bit pow/mod, (g**pr) % m
    unsigned long long result;
    unsigned long long gl;
    unsigned long long ml;

    gl = g;
    ml = this->DH_P;
    result = 1;
    while(pr)
    {
        //if set then do a multiple and modulus
        if(pr & 1)
            result = (result * gl) % ml;

        //bit shift
        pr >>= 1;
        gl = (gl * gl) % ml;
    };

    return result;
}

//return our value and set the provided value to what the other side is given
unsigned int MeshNetworkInternal::DHCreateChallenge(unsigned int *challenge)
{
    //select a random value
    unsigned int priv = esp_random();
    *challenge = this->DHPowMod(this->DH_G, priv);
    DEBUG_WRITE("DHCreateChallenge priv ");
    DEBUG_WRITEHEXVAL(priv, 8);
    DEBUG_WRITE(", challenge ");
    DEBUG_WRITEHEXVAL(*challenge, 8);
    DEBUG_WRITE("\n");
    return priv;
}

unsigned int MeshNetworkInternal::DHFinishChallenge(unsigned int priv, unsigned int challenge)
{
    unsigned int ret;

    ret = this->DHPowMod(challenge, priv);
    DEBUG_WRITE("DHFinishCHallenge priv ");
    DEBUG_WRITEHEXVAL(priv, 8);
    DEBUG_WRITE(", challenge ");
    DEBUG_WRITEHEXVAL(challenge, 8);
    DEBUG_WRITE(", ret ");
    DEBUG_WRITEHEXVAL(ret, 8);
    DEBUG_WRITE("\n");

    return ret;

}

int MeshNetworkInternal::DHInit(unsigned int P, unsigned int G)
{
    //if G is larger than P then fail
    if(G > P)
        return -1;

    this->DH_P = P;
    this->DH_G = G;

    DEBUG_WRITE("DHInit P ");
    DEBUG_WRITE(P);
    DEBUG_WRITE(", G ");
    DEBUG_WRITE(G);
    DEBUG_WRITE("\n");

    return 0;
}
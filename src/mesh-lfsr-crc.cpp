#include "mesh_internal.h"
#include <unistd.h>

int MeshNetworkInternal::SetBroadcastLFSR(unsigned int BroadcastLFSR, uint8_t Mask[3])
{
    int Divisor;

    //none of them can be 32 or above
    if((Mask[0]| Mask[1] | Mask[2]) & 0xe0)
        return -1;

    //check if each of the masks are valid, we know mask 4 is 32 so check if all of the numbers can be divided by
    //2, 4, 8, 16 to determine if they are co-prime
    for(Divisor = 16; Divisor > 1; Divisor >>= 1)
    {
        //if we can divide all numbers by one of the common multiples of 32 then it isn't co-prime
        if(((Mask[0] % Divisor) == 0) && ((Mask[1] % Divisor) == 0) && ((Mask[2] % Divisor) == 0))
            return -1;
    }

    //set our broadcast LFSR and determine our bits to use
    this->LFSR_Broadcast = BroadcastLFSR;
    this->LFSR_BroadcastMask = 0x80000000 | (1 << (Mask[0] - 1)) | (1 << (Mask[1] - 1)) | (1 << (Mask[2] - 1));
    this->BroadcastMsgID = 0;
    return 0;
}

unsigned int MeshNetworkInternal::CreateLFSRMask()
{
    //generate a random mask
    unsigned int NewMask;
    uint8_t BitCount;
    uint8_t DivMatch;
    uint8_t Pos;
    uint8_t Divisor;
    uint8_t SelectedBit[5];
    unsigned int RandVal;
    unsigned int XNor;

    //loop until we have some number of bits selected that
    //only have a common denominator of 1. Bits are offset by 1 in the check so 1 to 32 vs 0 to 31
    //or with 1 as last entry is 32 which is even

    //select 4 or 6 bits to work with and setup or xnor flag
    XNor = esp_random();
    BitCount = (((XNor & 1) << 1) | 4) - 1;
    XNor = (XNor & 0x40);

    //cycle through until we fill in all masks with values that have no common denominator
    RandVal = 0;
    while(1)
    {
        //fill in each random position
        for(Pos = 0; Pos < BitCount; Pos++)
        {
            //if we are low on values then pull a new random value
            if((RandVal & ~0x1f) == 0)
                RandVal = esp_random();

            SelectedBit[Pos] = (RandVal & 0x1f);
            RandVal >>= 5;
        }

        //make sure no 2 numbers are the same
        DivMatch = 0;
        for(Pos = 0; Pos < (BitCount - 1); Pos++)
        {
            //if match an entry then fail
            for(int Pos2 = Pos+1; Pos2 < BitCount; Pos2++)
            {
                if(SelectedBit[Pos] == SelectedBit[Pos2])
                {
                    DivMatch = 1;
                    break;
                }
            }
            if(DivMatch)
                break;
        }

        //if we didn't get through all the entries then try again
        if(DivMatch)
            continue;

        //check if each of the masks are valid, we know mask 4 is 32 so check if all of the numbers can be divided by
        //2, 4, 8, 16 to determine if they are co-prime
        for(Divisor = 16; Divisor > 1; Divisor >>= 1)
        {
            //if we can divide all numbers by one of the common multiples of 32 then it isn't co-prime
            DivMatch = 0;
            for(Pos = 0; Pos < BitCount; Pos++)
            {
                if((SelectedBit[Pos] % Divisor) == 0)
                    DivMatch++;
            }

            if(DivMatch == BitCount)
                break;
        }

        //if DivMatch does not match our bitcount then break out of the loop
        if(DivMatch != BitCount)
            break;
    };

    //setup our new mask
    NewMask = 0x1f | XNor;

    //cycle and add the others selections
    for(Pos = 0; Pos < BitCount; Pos++)
    {
        NewMask <<= 5;
        NewMask |= (SelectedBit[Pos] - 1);
    }

    //all done
    return NewMask;
}

unsigned int MeshNetworkInternal::CalculateLFSR(unsigned int LFSR, unsigned int Mask)
{
    //calculate a new LFSR value
    unsigned int NewLFSR;
    unsigned int BitOffset;
    unsigned int XNOR = Mask >> 31;
    unsigned int CurMask;
    unsigned int Count;

    //cycle 8 bits on the LFSR
    NewLFSR = LFSR >> 31;
    for(Count = 0; Count < 8; Count++)
    {
        //cycle through the mask and calculate a new LFSR value
        //we do not reload NewLFSR as it has it's bit set at the end of the loop
        CurMask = Mask & 0x7fffffff;
        while(CurMask)
        {
            BitOffset = CurMask & 0x1f;
            NewLFSR ^= ((LFSR >> BitOffset) & 1) ^ XNOR;
            CurMask >>= 5;
        };

        NewLFSR &= 1;   //safety just in-case
        LFSR = (LFSR >> 1) | (NewLFSR << 31);
    }

    //if our final value is 0 or 0xffffffff then change the value to 1
    if(!LFSR || (LFSR == 0xffffffff))
        LFSR = 1;

    return LFSR;
}

uint8_t MeshNetworkInternal::CalculateCRC(const void *Data, unsigned int DataLen)
{
    return this->CalculateCRC(Data, DataLen, 0xff);
}

uint8_t MeshNetworkInternal::CalculateCRC(const void *Data, unsigned int DataLen, uint8_t StartCRC)
{
    uint8_t Count;
    uint16_t CRC;
    uint8_t *InData = (uint8_t *)Data;
    uint8_t BitPos;

    //calculate our index, do a poor-man 8 bit crc
    CRC = StartCRC;
    for(Count = 0; Count < DataLen; Count++)
    {
        CRC ^= (uint16_t)InData[Count] << 8;
        for(BitPos = 8; BitPos; BitPos--) {
            if(CRC & 0x8000)
                CRC ^= 0x8380;
            CRC <<=1;
        }
    }

    //return the crc
    return (CRC >> 8);
}

unsigned int MeshNetworkInternal::PermuteBroadcastLFSR(const uint8_t *MAC, unsigned int ID)
{
    //start with our LFSR for global, run it through a few cycles with the MAC and use the result as our new LFSR for messages
    //from that device

    unsigned int LFSR;
    union {
        uint8_t Data[4];
        unsigned int Ret;
    };

    //as with crypt, look to see if aes function on esp is 1 or multiple rounds
    //allowing simplistic data modification for permutation at lower impact potentially
    //yes I could use crc32 and be done with it, this is more fun
    LFSR = this->LFSR_Broadcast;
    Data[0] = this->CalculateCRC(MAC, 6, LFSR & 0xff);
    Data[0] = this->CalculateCRC(&ID, 4, Data[0]);
    Data[1] = this->CalculateCRC(MAC, 6, Data[0] ^ ((LFSR >> 8) & 0xff));
    Data[1] = this->CalculateCRC(&ID, 4, Data[1]);
    Data[2] = this->CalculateCRC(MAC, 6, Data[1] ^ ((LFSR >> 16) & 0xff));
    Data[2] = this->CalculateCRC(&ID, 4, Data[2]);
    Data[3] = this->CalculateCRC(MAC, 6, Data[2] ^ ((LFSR >> 24) & 0xff));
    Data[3] = this->CalculateCRC(&ID, 4, Data[3]);

    //return our modified LFSR value
    return Ret;
}
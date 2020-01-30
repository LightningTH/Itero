#include "mesh_internal.h"
#include <unistd.h>

int MeshNetworkInternal::SetBroadcastLFSR(unsigned int BroadcastLFSR[2], uint8_t Mask1[3], uint8_t Mask2[3])
{
    int Divisor;

    //none of them can be 32 or above
    if(((Mask1[0]| Mask1[1] | Mask1[2]) | (Mask2[0]| Mask2[1] | Mask2[2])) & 0xe0)
        return -1;

    //if 0 then fail
    if(!Mask1[0] || !Mask1[1] || !Mask1[2] || !Mask2[0] || !Mask2[1] || !Mask2[2])
        return -1;

    //check if each of the masks are valid, we know mask 4 is 32 so check if all of the numbers can be divided by
    //2, 4, 8, 16 to determine if they are co-prime
    for(Divisor = 16; Divisor > 1; Divisor >>= 1)
    {
        //if we can divide all numbers by one of the common multiples of 32 then it isn't co-prime
        if((((Mask1[0] % Divisor) == 0) && ((Mask1[1] % Divisor) == 0) && ((Mask1[2] % Divisor) == 0)) ||
           (((Mask2[0] % Divisor) == 0) && ((Mask2[1] % Divisor) == 0) && ((Mask2[2] % Divisor) == 0)))
            return -1;
    }

    //make sure none of the bits overlap
    if((Mask1[0] == Mask1[1]) || (Mask1[0] == Mask1[2]) || (Mask1[1] == Mask1[2]) ||
       (Mask2[0] == Mask2[1]) || (Mask2[0] == Mask2[2]) || (Mask2[1] == Mask2[2]))
    {
        return -1;
    }

    //set our broadcast LFSR and determine our bits to use
    this->LFSR_Broadcast.LFSR = BroadcastLFSR[0];
    this->LFSR_Broadcast.LFSRRot = BroadcastLFSR[1];
    this->LFSR_Broadcast.LFSRMask = 0x3e000000 | ((Mask1[0] - 1) << 20) | ((Mask1[1] - 1) << 15) | ((Mask1[2] - 1) << 10);
    this->LFSR_Broadcast.LFSRRotMask = 0x3e000000 | ((Mask2[0] - 1) << 20) | ((Mask2[1] - 1) << 15) | ((Mask2[2] - 1) << 10);
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
    //note we do a bitcount of 3 or 5 as the last entry is always 31
    XNor = esp_random();
    BitCount = (((XNor & 0x80) >> 6) | 4) - 1;
    XNor = (XNor & 0xC0);

    //cycle through until we fill in all masks with values that have no common denominator
    RandVal = 0;
    while(1)
    {
        //fill in each random position even the unused entries if only 4 slots are selected
        for(Pos = 0; Pos < sizeof(SelectedBit); Pos++)
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

        //check if each of the masks are valid, we know the last mask is 32 so check if all of the numbers can be divided by
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

    //setup our new mask, shift XNor by a bit as the final shifts will put us in the location we want
    NewMask = 0x1f | (XNor >> 1);

    //cycle and add the others selections, when only 4 are used the last 2 are unused which is ok
    for(Pos = 0; Pos < sizeof(SelectedBit); Pos++)
    {
        NewMask <<= 5;
        NewMask |= (SelectedBit[Pos] - 1);
    }

    //all done
    return NewMask;
}

unsigned int MeshNetworkInternal::RotateLFSR(unsigned int LFSR, unsigned int Mask, unsigned int Count)
{
    //calculate a new LFSR value
    unsigned int NewLFSR;
    unsigned int BitOffset;
    unsigned int XNOR = (Mask >> 30) & 1;
    unsigned int BitCountFlag = (Mask >> 31);
    unsigned int CurMask;
    unsigned int BitCount;
    unsigned int StartMask;
    unsigned char StartBitCount;

    //figure out the start mask
    StartMask = Mask & 0x3fffffff;
    StartBitCount = 6;
    if(!BitCountFlag)
    {
        StartMask >>= 10;
        StartBitCount = 4;
    }

    //cycle x bits on the LFSR
    NewLFSR = LFSR >> 31;
    while(Count)
    {
        Count--;

        //cycle through the mask and calculate a new LFSR value
        //we do not reload NewLFSR as it has it's bit set at the end of the loop
        CurMask = StartMask;
        BitCount = StartBitCount;

        //process the mask
        while(BitCount)
        {
            BitOffset = CurMask & 0x1f;
            NewLFSR ^= (LFSR >> BitOffset);
            CurMask >>= 5;
            BitCount--;
        };

        NewLFSR = (NewLFSR ^ XNOR) & 1;
        LFSR = (LFSR >> 1) | (NewLFSR << 31);
    }

    //if our final value is 0 or 0xffffffff then change the value to 1
    if(!LFSR || (LFSR == 0xffffffff))
        LFSR = 1;

    return LFSR;
}

void MeshNetworkInternal::CalculateLFSR(LFSRStruct *LFSR)
{
    //determine how much we will rotate the LFSRRot and the LFSR itself
    unsigned char ROT[2];

    //1 to 16 for LFSR, 1 to 16 for LFSRRot so it is unknown if we do or don't mix in it's rotation into the LFSR rotation
    ROT[0] = (LFSR->LFSRRot & 0xf) + 1;
    ROT[1] = ((LFSR->LFSRRot >> 7) & 0xf) + 1;

    //now rotate and calculate new values for LFSR and LFSRRot
    LFSR->LFSRRot = this->RotateLFSR(LFSR->LFSRRot, LFSR->LFSRRotMask, ROT[1]);
    LFSR->LFSR = this->RotateLFSR(LFSR->LFSR, LFSR->LFSRMask, ROT[0]);
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

void MeshNetworkInternal::PermuteBroadcastLFSR(const uint8_t *MAC, unsigned int ID, LFSRStruct *LFSR)
{
    //start with our LFSR for global, run it through a few cycles with the MAC and use the result as our new LFSR for messages
    //from that device
    union {
        uint8_t Data[4];
        unsigned int Ret;
    };

    //as with crypt, look to see if aes function on esp is 1 or multiple rounds
    //allowing simplistic data modification for permutation at lower impact potentially
    //yes I could use crc32 and be done with it, this is more fun
    LFSR->LFSR = this->LFSR_Broadcast.LFSR;
    Data[0] = this->CalculateCRC(MAC, 6, LFSR->LFSR & 0xff);
    Data[0] = this->CalculateCRC(&ID, 4, Data[0]);
    Data[1] = this->CalculateCRC(MAC, 6, Data[0] ^ ((LFSR->LFSR >> 8) & 0xff));
    Data[1] = this->CalculateCRC(&ID, 4, Data[1]);
    Data[2] = this->CalculateCRC(MAC, 6, Data[1] ^ ((LFSR->LFSR >> 16) & 0xff));
    Data[2] = this->CalculateCRC(&ID, 4, Data[2]);
    Data[3] = this->CalculateCRC(MAC, 6, Data[2] ^ ((LFSR->LFSR >> 24) & 0xff));
    Data[3] = this->CalculateCRC(&ID, 4, Data[3]);

    LFSR->LFSRRot = this->LFSR_Broadcast.LFSRRot;
    Data[0] = this->CalculateCRC(MAC, 6, LFSR->LFSRRot & 0xff);
    Data[0] = this->CalculateCRC(&ID, 4, Data[0]);
    Data[1] = this->CalculateCRC(MAC, 6, Data[0] ^ ((LFSR->LFSRRot >> 8) & 0xff));
    Data[1] = this->CalculateCRC(&ID, 4, Data[1]);
    Data[2] = this->CalculateCRC(MAC, 6, Data[1] ^ ((LFSR->LFSRRot >> 16) & 0xff));
    Data[2] = this->CalculateCRC(&ID, 4, Data[2]);
    Data[3] = this->CalculateCRC(MAC, 6, Data[2] ^ ((LFSR->LFSRRot >> 24) & 0xff));
    Data[3] = this->CalculateCRC(&ID, 4, Data[3]);

    //setup our modified value
    LFSR->LFSRRot = Ret;

    //put in our masks
    LFSR->LFSRMask = this->LFSR_Broadcast.LFSRMask;
    LFSR->LFSRRotMask = this->LFSR_Broadcast.LFSRRotMask;
}
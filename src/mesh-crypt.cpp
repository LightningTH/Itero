#include "mesh_internal.h"
#include "debug.h"
#include <unistd.h>

void MeshNetworkInternal::Encrypt(const void *InData, void *OutData, unsigned short DataLen, LFSRStruct *LFSR)
{
    int Count;
    uint8_t *IntInData = (uint8_t *)InData;
    uint8_t *IntOutData = (uint8_t *)OutData;
    uint8_t InChar;

    //cycle through and encrypt, we do a cbc style mode by xor'ing in the original data into the LFSR before it is recalculated
    for(Count = 0; Count < DataLen; Count++, IntInData++, IntOutData++)
    {
        //get a copy of the character just in-case in and out are the same
        InChar = *IntInData;
        *IntOutData = *IntInData ^ (LFSR->LFSR & 0xff);

        //advance the LFSR after xor'ing in the original data to force a CBC mode
        LFSR->LFSR ^= InChar;
        LFSR->LFSRRot ^= (uint32_t)InChar << 13;
        this->CalculateLFSR(LFSR);
    }

    return;
}

void MeshNetworkInternal::Decrypt(const void *InData, void *OutData, unsigned short DataLen, LFSRStruct *LFSR)
{
    int Count;
    uint8_t *IntInData = (uint8_t *)InData;
    uint8_t *IntOutData = (uint8_t *)OutData;

    //cycle through and encrypt, we do a cbc style mode by xor'ing in the original data into the LFSR before it is recalculated
    for(Count = 0; Count < DataLen; Count++, IntInData++, IntOutData++)
    {
        *IntOutData = *IntInData ^ (LFSR->LFSR & 0xff);

        //advance the LFSR after xor'ing in the resulting data which should be the original to force CBC mode
        LFSR->LFSR ^= *IntOutData;
        LFSR->LFSRRot ^= (uint32_t)*IntOutData << 13;
        this->CalculateLFSR(LFSR);
    }

    return;
}

uint8_t *MeshNetworkInternal::EncryptPacketCommon(unsigned int SequenceID, LFSRStruct *LFSR, const uint8_t *InData, unsigned short DataLen, unsigned short *OutPacketLen)
{
    unsigned int ValidPacketID = VALID_PACKET_ID;
    uint8_t *Packet;
    PacketHeaderStruct *PacketHeader;

    //if no room for the header due to wrap around then fail
    if(!DataLen || ((DataLen + sizeof(PacketHeaderStruct)) < DataLen))
        return 0;

    if((DataLen + sizeof(PacketHeaderStruct)) > MAX_PACKET_SIZE)
        return 0;

    //allocate space for the packet
    Packet = (uint8_t *)malloc(DataLen + sizeof(PacketHeaderStruct) + sizeof(ValidPacketID));
    PacketHeader = (PacketHeaderStruct *)Packet;

    //setup the header
    PacketHeader->SequenceID = SequenceID;
    PacketHeader->InternalCRC = this->CalculateCRC(&Packet[1], sizeof(PacketHeaderStruct) - 1);   //calculate the CRC with the ID and ValidID in it
    PacketHeader->InternalCRC = this->CalculateCRC(InData, DataLen, PacketHeader->InternalCRC); //add in the incoming data

    //do the encryption
    this->Encrypt(InData, &Packet[sizeof(PacketHeaderStruct)], DataLen, LFSR);

    //add on the indicator that the packet was decrypted properly
    this->Encrypt(&ValidPacketID, &Packet[sizeof(PacketHeaderStruct) + DataLen], sizeof(ValidPacketID), LFSR);

    //everything is good, return the buffer, ack will cause LFSR and ID to change
    *OutPacketLen = DataLen + sizeof(PacketHeaderStruct) + sizeof(ValidPacketID);
    return Packet;
}

uint8_t *MeshNetworkInternal::EncryptPacket(KnownDeviceStruct *Device, const uint8_t *InData, unsigned short DataLen, unsigned short *OutPacketLen)
{
    uint8_t *ret;
    LFSRStruct LFSR;

    //copy off the LFSR so that we can update if successful
    DEBUG_WRITE("EncryptPacket: LFSR: ");
    DEBUG_WRITEHEXVAL(Device->LFSR_Out.LFSR, 8);
    DEBUG_WRITE(", Mask: ");
    DEBUG_WRITEHEXVAL(Device->LFSR_Out.LFSRMask, 8);
    DEBUG_WRITE(", ROT: ");
    DEBUG_WRITEHEXVAL(Device->LFSR_Out.LFSRRot, 8);
    DEBUG_WRITE(", RotMask: ");
    DEBUG_WRITEHEXVAL(Device->LFSR_Out.LFSRRotMask, 8);
    DEBUG_WRITE("\n");

    LFSR = Device->LFSR_Out;
    ret = this->EncryptPacketCommon(Device->ID_Out, &LFSR, InData, DataLen, OutPacketLen);
    if(ret)
    {
        //update LFSR and ID
        Device->LFSR_OutPrev = Device->LFSR_Out;
        Device->LFSR_Out = LFSR;
        Device->ID_Out++;
    }

    return ret;
}

uint8_t *MeshNetworkInternal::EncryptBroadcastPacket(const uint8_t *InData, unsigned short DataLen, unsigned short *OutPacketLen)
{
    uint8_t *ret;
    LFSRStruct LFSR;

    //get our LFSR for this device
    this->PermuteBroadcastLFSR(this->MAC, this->BroadcastMsgID, &LFSR);
    DEBUG_WRITE("Encrypt Broadcast LFSR: ");
    DEBUG_WRITEHEXVAL(LFSR.LFSR, 8);
    DEBUG_WRITE(", Mask: ");
    DEBUG_WRITEHEXVAL(LFSR.LFSRMask, 8);
    DEBUG_WRITE(", ROT: ");
    DEBUG_WRITEHEXVAL(LFSR.LFSRRot, 8);
    DEBUG_WRITE(", RotMask: ");
    DEBUG_WRITEHEXVAL(LFSR.LFSRRotMask, 8);
    DEBUG_WRITE(", ID: ");
    DEBUG_WRITEHEXVAL(this->BroadcastMsgID, 8);
    DEBUG_WRITE("\n");

    ret = this->EncryptPacketCommon(this->BroadcastMsgID, &LFSR, InData, DataLen, OutPacketLen);
    if(ret)
    {
        this->BroadcastMsgID++;
        this->prefs->begin("mesh");
        this->prefs->putUInt("broadcastid", this->BroadcastMsgID);
        this->prefs->end();
    }

    return ret;
}

uint8_t *MeshNetworkInternal::DecryptPacketCommon(unsigned int SequenceID, LFSRStruct *LFSR, const uint8_t *InPacket, unsigned short PacketLen, unsigned short *OutDataLen)
{
    uint8_t *Packet;
    PacketHeaderStruct *PacketHeader;
    uint8_t CRC;
    unsigned short DataSize;
    unsigned int ValidPacketID;

    //decrypt data and return the data found in the packet

    //see if there is anything to handle
    if(PacketLen < (sizeof(PacketHeaderStruct) + sizeof(ValidPacketID)))
        return 0;

    //see if the ID is what we expect
    PacketHeader = (PacketHeaderStruct *)InPacket;
    if(PacketHeader->SequenceID != SequenceID)
        return 0;

    //see if we can decrypt it
    DataSize = PacketLen - sizeof(PacketHeaderStruct) - sizeof(ValidPacketID);
    if(!DataSize)
        return 0;

    Packet = (uint8_t *)malloc(DataSize);
    this->Decrypt(&InPacket[sizeof(PacketHeaderStruct)], Packet, DataSize, LFSR);
    this->Decrypt(&InPacket[sizeof(PacketHeaderStruct) + DataSize], (uint8_t *)&ValidPacketID, sizeof(ValidPacketID), LFSR);

    //check the internal CRC and PacketID
    CRC = this->CalculateCRC(&InPacket[1], sizeof(PacketHeaderStruct) - 1);

    DEBUG_WRITE("Decrypt results: ");
    DEBUG_WRITEHEXVAL(ValidPacketID, 8);
    DEBUG_WRITE(" == ");
    DEBUG_WRITEHEXVAL(VALID_PACKET_ID, 8);
    DEBUG_WRITE(", CRC ");
    DEBUG_WRITEHEXVAL(this->CalculateCRC(Packet, DataSize, CRC), 2);
    DEBUG_WRITE(" == ");
    DEBUG_WRITEHEXVAL(PacketHeader->InternalCRC, 2);
    DEBUG_WRITE(", Data size:");
    DEBUG_WRITE(DataSize);
    DEBUG_WRITE("\n");

    if((ValidPacketID != VALID_PACKET_ID) || (this->CalculateCRC(Packet, DataSize, CRC) != PacketHeader->InternalCRC))
    {
        free(Packet);
        return 0;
    }

    //everything is good
    *OutDataLen = DataSize;
    return Packet;
}

uint8_t *MeshNetworkInternal::DecryptPacket(KnownDeviceStruct *Device, const uint8_t *InPacket, unsigned short PacketLen, unsigned short *OutDataLen, unsigned int *DoAck)
{
    uint8_t *ret;
    LFSRStruct LFSR;

    LFSR = Device->LFSR_In;
    ret = this->DecryptPacketCommon(Device->ID_In, &LFSR, InPacket, PacketLen, OutDataLen);

    //if a good message then increment the ID
    *DoAck = 0;
    if(ret)
    {
        *DoAck = 1;
        Device->LFSR_InPrev = Device->LFSR_In;
        Device->LFSR_In = LFSR;
        Device->ID_In++;
    }
    else
    {
        //we failed to decrypt, try to decrypt with the previous lfsr and see if we are off by 1
        LFSR = Device->LFSR_InPrev;
        ret = this->DecryptPacketCommon(Device->ID_In - 1, &LFSR, InPacket, PacketLen, OutDataLen);

        //if successful then indicate we need to just re-ack
        if(ret)
        {
            *DoAck = 1;
            free(ret);
            ret = 0;
        }
    }
    
    return ret;
}

uint8_t *MeshNetworkInternal::DecryptBroadcastPacket(UnknownDeviceStruct *Device, const uint8_t *InPacket, unsigned short PacketLen, unsigned short *OutDataLen)
{
    unsigned int SequenceID;
    LFSRStruct LFSR;

    //decrypt data and return the data found in the packet

    //see if there is anything to handle
    if(PacketLen < sizeof(PacketHeaderStruct))
        return 0;

    //get our sequence ID
    SequenceID = ((PacketHeaderStruct *)InPacket)->SequenceID;

    //get our LFSR for this device
    this->PermuteBroadcastLFSR(Device->MAC, SequenceID, &LFSR);

    //attempt to decrypt
    return this->DecryptPacketCommon(SequenceID, &LFSR, InPacket, PacketLen, OutDataLen);
}
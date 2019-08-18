#include <Arduino.h>
#include "mesh_internal.h"
#include "mesh.h"
#include <esp_wifi.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

void MeshNetworkInternal::Ping()
{
    //send a ping out to see who is around
    this->SendPayload(MSG_Ping, this->BroadcastMAC, 0, 0);
}

int MeshNetworkInternal::Write(const uint8_t MAC[MAC_SIZE], const uint8_t *Data, unsigned short DataLen)
{
    int Ret;
    uint8_t *EncData;
    unsigned short EncLen;
    KnownDeviceStruct *Device;
    int BroadcastMsg = 0;

    if(!this->Initialized)
        return MeshWriteErrors::MeshNotInitialized;

    if((DataLen + sizeof(PacketHeaderStruct)) > MAX_PACKET_SIZE)
        return MeshWriteErrors::DataTooLarge;

    //encrypt the data with the proper LFSR then send it along
    if(memcmp(MAC, this->BroadcastMAC, MAC_SIZE) == 0)
    {
        DEBUG_WRITELN("Sending broadcast message");
        EncData = this->EncryptBroadcastPacket(Data, DataLen, &EncLen);

        //if no valid data then fail
        if(!EncData)
            return MeshWriteErrors::DataTooLarge;

        BroadcastMsg = 1;
    }
    else
    {
        //look up and encrypt with the proper device
        Device = this->FindKnownDevice(MAC);
        if(!Device)
            return MeshWriteErrors::DeviceDoesNotExist;

        //if we have an outstanding message then error
        if(Device->LastOutMessage)
            return MeshWriteErrors::PreviousWriteNotComplete;

        DEBUG_WRITE("Sending data to ");
        DEBUG_WRITEMAC(MAC);
        DEBUG_WRITE("\n");

        //store it off in-case we need to re-transmit
        Device->LastOutMessage = (uint8_t *)malloc(DataLen);
        if(!Device->LastOutMessage)
            return MeshWriteErrors::OutOfMemory;
        memcpy(Device->LastOutMessage, Data, DataLen);
        Device->LastOutMessageLen = DataLen;
        Device->LastOutMessageCheck = 0;

        //if we are in reset mode then tell the other side we want to reconnect
        if(Device->ConnectState == CS_Reset)
        {
            this->Connect(Device->MAC);
            this->MessageWasSent = 1;
            return MeshWriteErrors::ResettingConnection;
        }

        //encrypt the packet
        EncData = this->EncryptPacket(Device, Data, DataLen, &EncLen);

        //if no valid data then fail
        if(!EncData)
        {
            free(Device->LastOutMessage);
            Device->LastOutMessage = 0;
            return MeshWriteErrors::DataTooLarge;
        }

        DEBUG_WRITE("EncData ptr: ");
        DEBUG_WRITEHEXVAL(EncData, 8);
        DEBUG_WRITE("\n");
        DEBUG_DUMPHEX("EncPacket:", EncData, EncLen);
    }
    
    //send the data and return if it succeeded    
    Ret = this->SendPayload(MSG_Message, MAC, EncData, EncLen);

    //if a broadcast message then free the packet buffer
    if(BroadcastMsg)
        free(EncData);
    else
        this->MessageWasSent = 1;

    return Ret;
}

void *Static_ResendMessages(void *)
{
    if(_GlobalMesh)
        _GlobalMesh->ResendMessages();

    return 0;
}

void MeshNetworkInternal::ResendMessages()
{
    KnownDeviceStruct *CurDevice;
    int i;
    int FoundMessage;

    while(1)
    {
        //cycle through all known connections and see if there is anything we need to send out
        if(this->MessageWasSent)
        {
            FoundMessage = 0;
            for(i = 0; i < TABLE_SIZE; i++)
            {
                CurDevice = this->KnownDeviceTable[i];
                while(CurDevice)
                {
                    //if we have a message increment the check value
                    if(CurDevice->LastOutMessage)
                    {
                        //set our flag so we can keep checking but only send if we are connected and not in reset
                        FoundMessage = 1;
                        if(CurDevice->ConnectState == ConnectStateEnum::CS_ResetConnecting)
                        {
                            CurDevice->LastOutMessageCheck++;
                            if(CurDevice->LastOutMessageCheck >= 5) //2.5 seconds due to 500ms delay
                            {
                                //taken too long reset connect state
                                CurDevice->ConnectState = ConnectStateEnum::CS_Reset;
                                if(this->SendFailedCallback)
                                    this->SendFailedCallback(CurDevice->MAC);
                            }
                        }
                        else if(CurDevice->ConnectState == ConnectStateEnum::CS_Connected)
                        {
                            CurDevice->LastOutMessageCheck++;
                            if((CurDevice->LastOutMessageCheck & 1) == 0)
                            {
                                //if we have waited up to 5 cycles then stop waiting (2.5 seconds)
                                if(CurDevice->LastOutMessageCheck >= 5)
                                {
                                    free(CurDevice->LastOutMessage);
                                    CurDevice->LastOutMessage = 0;
                                    CurDevice->LastOutMessageCheck = 0;
                                    CurDevice->LastOutMessageLen = 0;

                                    if(this->SendFailedCallback)
                                        this->SendFailedCallback(CurDevice->MAC);
                                }
                            }
                            else
                            {
                                DEBUG_WRITE((unsigned long) (esp_timer_get_time() / 1000ULL));
                                DEBUG_WRITE(": Message sent to ");
                                DEBUG_WRITEMAC(CurDevice->MAC);
                                DEBUG_WRITE(" being resent, len ");
                                DEBUG_WRITE(CurDevice->LastOutMessageLen);
                                DEBUG_WRITE("\n");

                                //reset the out lfsr and out id
                                CurDevice->LFSR_Out = CurDevice->LFSR_OutPrev;
                                CurDevice->ID_Out--;

                                //encrypt the packet
                                unsigned short EncLen;
                                uint8_t *EncData = this->EncryptPacket(CurDevice, CurDevice->LastOutMessage, CurDevice->LastOutMessageLen, &EncLen);
                                if(EncData)
                                {
                                    //resend, we will retransmit every 1 seconds
                                    this->SendPayload(MSG_Message, CurDevice->MAC, EncData, EncLen);
                                    free(EncData);
                                }
                            }
                        }
                    }

                    //next device
                    CurDevice = CurDevice->Next;
                }
            }

            //if no messages then reset the send flag
            if(!FoundMessage)
                this->MessageWasSent = 0;
        }
        delay(500);
    };

    return;
}

int MeshNetworkInternal::SendPayload(MessageTypeEnum MsgType, const uint8_t *MAC, const void *InData, unsigned short DataLen)
{
    //just send a payload as-is, no encryption is done!
    uint8_t *FinalPayload;
    uint8_t *Data = (uint8_t *)InData;
    WifiHeaderStruct *Header;
    int ret;

    //make sure the payload can fit
    if(((int)DataLen + sizeof(WifiHeaderStruct)) > MAX_PACKET_SIZE)
        return -1;

    FinalPayload = (uint8_t *) malloc(DataLen + sizeof(WifiHeaderStruct));
    if(!FinalPayload)
        return -1;

    //copy the data into our payload packet
    memcpy(&FinalPayload[sizeof(WifiHeaderStruct)], Data, DataLen);

    //setup the Header
    Header = (WifiHeaderStruct *)FinalPayload;
    memset(Header, 0, sizeof(WifiHeaderStruct));
    Header->FC = 0x00d0;
    Header->Duration = 0;
    Header->Type = MsgType;

    //setup the mac values
    memcpy(Header->MAC_Sender, this->MAC, MAC_SIZE);
    memcpy(Header->MAC_Reciever, MAC, MAC_SIZE);

    DEBUG_WRITE("Sending ");
    DEBUG_WRITE(DataLen + sizeof(WifiHeaderStruct));
    DEBUG_WRITE(" bytes to ");
    DEBUG_WRITEMAC(MAC);
    DEBUG_WRITE("\n");
    DEBUG_DUMPHEX(0, FinalPayload, DataLen + sizeof(WifiHeaderStruct));

    //transmit the raw packet
    ret = esp_wifi_80211_tx(WIFI_IF_STA, FinalPayload, DataLen + sizeof(WifiHeaderStruct), false);
    free(FinalPayload);
    if(ret != ESP_OK)
    {
        DEBUG_WRITE("Error on esp_wifi_80211_tx: ");
        DEBUG_WRITE(ret);
        DEBUG_WRITE("\n");
        return -1;
    }
    else
    {
        DEBUG_WRITE("esp_wifi_80211_tx successful");
        DEBUG_WRITE("\n");
    }
    
    //all good
    return 0;
}
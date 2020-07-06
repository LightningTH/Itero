#include <Arduino.h>
#include "mesh_internal.h"
#include "mesh.h"
#include <esp_wifi.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

pthread_mutex_t mesh_message_lock;

void *Static_ProcessRXMessages(void *)
{
    pthread_mutex_init(&mesh_message_lock, NULL);
    if(_GlobalMesh)
        _GlobalMesh->ProcessRXMessages();

    return 0;
}

void MeshNetworkInternal::ProcessRXMessages()
{
    MeshMessageStruct *CurMessage;

    while(1)
    {
        //if no beginning message then wait
        if(!this->MeshMessageBegin)
        {
            yield();
            delay(500);
            continue;
        }

        //we have a beginning message, pull it off for processing
        pthread_mutex_lock(&mesh_message_lock);

        CurMessage = this->MeshMessageBegin;
        this->MeshMessageBegin = CurMessage->next;

        //if we are the tail then wipe it out
        if(this->MeshMessageTail == CurMessage)
            this->MeshMessageTail = 0;

        //unlock as we are done
        pthread_mutex_unlock(&mesh_message_lock);

        //now process the message
        this->HandleRXMessage(CurMessage->Message, CurMessage->Len, CurMessage->Count);
        free(CurMessage);

        //yield before trying another message
        yield();
    }
}

void Promiscuous_RX(void *buf, wifi_promiscuous_pkt_type_t type)
{
    //if we hav a global mesh and it's flag is set for broadcasting then allow promiscuous to process the message
    if(_GlobalMesh && _GlobalMesh->CanBroadcast())
        _GlobalMesh->PromiscuousRX(buf, type);
}

void MeshNetworkInternal::PromiscuousRX(void *buf, wifi_promiscuous_pkt_type_t type)
{
    wifi_promiscuous_pkt_t *packet = (wifi_promiscuous_pkt_t *)buf;
    uint8_t *Payload = (uint8_t *)packet->payload;
    WifiHeaderStruct *WifiHeader = (WifiHeaderStruct *)Payload;
    MeshMessageStruct *CurMessage;

    //message length - header - crc
    uint16_t PayloadLen = packet->rx_ctrl.sig_len - 4;

    //if not action frame then return
    if(WifiHeader->FC != 0x00d0)
        return;

    //check the action type
    if((WifiHeader->Type & 0xF0) != 0x60)
        return;

    //probably a message we want to handle, add it to the list
    pthread_mutex_lock(&mesh_message_lock);

    //scan the list for an identical message, if the same sender and sequence id then increment count
    CurMessage = this->MeshMessageBegin;
    while(CurMessage)
    {
        //if the header matches then assume it is the same message and increment the count
        if(memcmp(CurMessage->Message, WifiHeader, sizeof(WifiHeaderStruct)) == 0)
        {
            CurMessage->Count++;

            //unlock as we are done
            pthread_mutex_unlock(&mesh_message_lock);
            return;
        }

        //next message
        CurMessage = CurMessage->next;
    }

    //no current message, create it
    CurMessage = (MeshMessageStruct *)malloc(sizeof(MeshMessageStruct) + PayloadLen);
    CurMessage->next = 0;
    CurMessage->Count = 0;
    CurMessage->Len = PayloadLen;
    memcpy(CurMessage->Message, Payload, PayloadLen);

    //add it to the end
    if(this->MeshMessageTail)
        this->MeshMessageTail->next = CurMessage;
    this->MeshMessageTail = CurMessage;

    //if no beginning then set it
    if(!this->MeshMessageBegin)
        this->MeshMessageBegin = CurMessage;

    //unlock as we are done
    pthread_mutex_unlock(&mesh_message_lock);
}

void MeshNetworkInternal::HandleRXMessage(uint8_t *Data, size_t DataLen, size_t Count)
{
    UnknownDeviceStruct *UnknownDevice;
    KnownDeviceStruct *KnownDevice;
    int NewDevice;
    uint8_t *DecryptedMessage;
    unsigned short DecryptedMessageLen;
    int BroadcastMsg;
    unsigned int AckID;
    uint8_t *EncData;
    unsigned short EncLen;

    WifiHeaderStruct *WifiHeader = (WifiHeaderStruct *)Data;
    uint8_t *Payload = &Data[sizeof(WifiHeaderStruct)];
    uint16_t PayloadLen = DataLen - sizeof(WifiHeaderStruct);

    //check for broadcast or directed at us
    if(memcmp(WifiHeader->MAC_Reciever, this->MAC, MAC_SIZE) == 0)
        BroadcastMsg = 0;
    else if(memcmp(WifiHeader->MAC_Reciever, this->BroadcastMAC, MAC_SIZE) == 0) {
        //if this mac matches us as the sender then ignore it as we don't need to re-send ourselves
        if(memcmp(WifiHeader->MAC_Sender, this->MAC, MAC_SIZE) == 0)
            return;

        //broadcast message
        BroadcastMsg = 1;
    }
    else
        //ignore the packet
        return;

    DEBUG_WRITE("Received ");
    if(BroadcastMsg) {
        DEBUG_WRITE("broadcast ");
    } else {
        DEBUG_WRITE("direct ");
    }
    DEBUG_WRITE("packet, type ");
    DEBUG_WRITEHEXVAL(WifiHeader->Type, 2);
    DEBUG_WRITE(", from MAC ");
    DEBUG_WRITEMAC(WifiHeader->MAC_Sender);
    DEBUG_WRITE(", len ");
    DEBUG_WRITE(PayloadLen);
    DEBUG_WRITE("\n");
    DEBUG_DUMPHEX(0, Payload, PayloadLen);

    //must be one of our actions, handle it accordingly
    switch(WifiHeader->Type)
    {
        case MSG_ConnectRequest:
            //if a broadcast message then ignore
            if(BroadcastMsg)
                return;

            this->ConnectRequest(WifiHeader->MAC_Sender, Payload, PayloadLen);
            break;

        case MSG_ConnHandshake:
            //if a broadcast message then ignore
            if(BroadcastMsg)
                return;

            this->ConnectHandshake(WifiHeader->MAC_Sender, Payload, PayloadLen);
            break;

        case MSG_Connected:
            //if a broadcast message then ignore
            if(BroadcastMsg)
                return;

            this->Connected(WifiHeader->MAC_Sender, Payload, PayloadLen);
            break;

        case MSG_Message:
            if(BroadcastMsg)
            {
                //broadcast message

                //see if we know of the device
                NewDevice = 0;
                UnknownDevice = this->FindUnknownDevice(WifiHeader->MAC_Sender);
                if(!UnknownDevice)
                {
                    //allocate memory for it
                    UnknownDevice = (UnknownDeviceStruct *)malloc(sizeof(UnknownDeviceStruct));
                    memcpy(UnknownDevice->MAC, &WifiHeader->MAC_Sender, MAC_SIZE);
                    UnknownDevice->ID = 0;
                    NewDevice = 1;
                }
                else
                {
                    //if the ID is below our current one then ignore it
                    if(((PacketHeaderStruct *)Payload)->SequenceID <= UnknownDevice->ID)
                        return;
                }

                //in theory we would wrap around at 0 however that requires 4 billion messages during the conference
                //or 12 messages/msec for 4 days straight

                //decrypt the message
                DecryptedMessage = this->DecryptBroadcastPacket(UnknownDevice, Payload, PayloadLen, &DecryptedMessageLen);
                if(!DecryptedMessage)
                {
                    if(NewDevice)
                        free(UnknownDevice);
                    return;
                }

                //store off the ID we found as everything decrypted properly
                UnknownDevice->ID = ((PacketHeaderStruct *)Payload)->SequenceID;

                //if a new device add it to our known list
                if(NewDevice)
                    this->InsertUnknownDevice(UnknownDevice);

                //all good, first rebroadcast this packet for others to see if we haven't seen enough copies
                if((Count < 3) && this->BroadcastFlag)
                {
                    //delay a random amount of time so we don't flood the wifi
                    delay((esp_random() & 0xff) + 1);   //0 to 265ms delay
                    yield();
                    esp_wifi_80211_tx(WIFI_IF_STA, Data, DataLen, 0);
                }

                //alert the callback
                if(this->BroadcastMessageCallback)
                    this->BroadcastMessageCallback(UnknownDevice->MAC, DecryptedMessage, DecryptedMessageLen);

                //free and continue
                free(DecryptedMessage);
            }
            else
            {
                //see if we know of the device
                NewDevice = 0;
                KnownDevice = this->FindKnownDevice(WifiHeader->MAC_Sender);
                if(!KnownDevice)
                    return;

                //if device is in reset mode then alert it
                if(KnownDevice->ConnectState == ConnectStateEnum::CS_Reset)
                {
                    this->Connect(KnownDevice->MAC);
                    return;
                }
                else if(KnownDevice->ConnectState == ConnectStateEnum::CS_ResetConnecting)
                    return;     //not ready yet, still connecting

                //in theory we would wrap around at 0 however that requires 4 billion messages during the conference
                //or 12 messages/msec for 4 days straight

                //decrypt the message
                DecryptedMessage = this->DecryptPacket(KnownDevice, Payload, PayloadLen, &DecryptedMessageLen, &AckID);

                DEBUG_WRITE("Message decrypt results: Ack ");
                DEBUG_WRITE(AckID);
                DEBUG_WRITE(", DecryptedMessage ");
                DEBUG_WRITEHEXVAL(DecryptedMessage, 8);
                DEBUG_WRITE("Callback ");
                DEBUG_WRITEHEXVAL(this->ReceiveMessageCallback, 8);
                DEBUG_WRITE("\n");

                //if an ack is required then send it
                if(AckID)
                {
                    //set the ack ID to respond to
                    AckID = KnownDevice->ID_In - 1;

                    //send an ack for this message
                    this->SendPayload(MSG_MessageAck, KnownDevice->MAC, &AckID, sizeof(AckID));
                }

                //if no message then don't do anything more
                if(!DecryptedMessage)
                    return;

                //alert the callback
                if(this->ReceiveMessageCallback)
                    this->ReceiveMessageCallback(KnownDevice->MAC, DecryptedMessage, DecryptedMessageLen);

                //free and continue
                free(DecryptedMessage);
            }
            break;

        case MSG_MessageAck:
            //if a broadcast message then ignore
            if(BroadcastMsg)
                return;

            //see if we know of the device
            NewDevice = 0;
            KnownDevice = this->FindKnownDevice(WifiHeader->MAC_Sender);
            if(!KnownDevice)
                return;

            //in theory we would wrap around at 0 however that requires 4 billion messages during the conference
            //or 12 messages/msec for 4 days straight

            //see if the ack is for the ID we expect
            if(*(unsigned int *)Payload == KnownDevice->ID_Out - 1)
            {
                //if we have a known message then remove it and reset our length
                if(KnownDevice->LastOutMessage)
                {
                    free(KnownDevice->LastOutMessage);
                    KnownDevice->LastOutMessage = 0;
                    KnownDevice->LastOutMessageLen = 0;
                    KnownDevice->LastOutMessageCheck = 0;

                    //alert the calling app to the message being received
                    if(this->ReceiveMessageCallback)
                        this->ReceiveMessageCallback(KnownDevice->MAC, 0, 0);
                }
            }
            break;

        case MSG_Ping:
            //if not a broadcast message then ignore
            if(!BroadcastMsg)
                return;

            //respond back with an ack directly to the requestor
            //while returning our nickname
            EncData = this->EncryptBroadcastPacket(this->PingData, this->PingDataLen, &EncLen);
            this->SendPayload(MSG_PingAck, WifiHeader->MAC_Sender, EncData, EncLen);
            free(EncData);
            break;

        case MSG_PingAck:
            //if a broadcast then ignore
            if(BroadcastMsg)
                return;

            DEBUG_WRITE("Ping ack from ");
            DEBUG_WRITEMAC(WifiHeader->MAC_Sender);
            DEBUG_WRITE("\n");

            UnknownDevice = this->FindUnknownDevice(WifiHeader->MAC_Sender);
            if(!UnknownDevice)
            {
                //allocate memory for it
                UnknownDevice = (UnknownDeviceStruct *)malloc(sizeof(UnknownDeviceStruct));
                memcpy(UnknownDevice->MAC, &WifiHeader->MAC_Sender, MAC_SIZE);
                UnknownDevice->ID = 0;
                NewDevice = 1;
            }
            else
            {
                //if the ID is below our current one then ignore it
                if(((PacketHeaderStruct *)Payload)->SequenceID <= UnknownDevice->ID)
                    return;
            }

            DecryptedMessage = this->DecryptBroadcastPacket(UnknownDevice, Payload, PayloadLen, &DecryptedMessageLen);
            if(!DecryptedMessage)
            {
                if(NewDevice)
                    free(UnknownDevice);
                return;
            }

            //store off the ID we found as everything decrypted properly
            UnknownDevice->ID = ((PacketHeaderStruct *)Payload)->SequenceID;

            //if a new device add it to our known list
            if(NewDevice)
                this->InsertUnknownDevice(UnknownDevice);

            //received an ack, report it
            if(this->PingCallback)
                this->PingCallback(WifiHeader->MAC_Sender, DecryptedMessage, DecryptedMessageLen);

            free(DecryptedMessage);
            break;

        case MSG_Disconnect:
            //if a broadcast message then ignore
            if(BroadcastMsg)
                return;

            //see if we know of the device
            NewDevice = 0;
            KnownDevice = this->FindKnownDevice(WifiHeader->MAC_Sender);
            if(!KnownDevice)
                return;

            //in theory we would wrap around at 0 however that requires 4 billion messages during the conference
            //or 12 messages/msec for 4 days straight

            //decrypt the message
            DecryptedMessage = this->DecryptPacket(KnownDevice, Payload, PayloadLen, &DecryptedMessageLen, &AckID);

            DEBUG_WRITE("Message decrypt results: Ack ");
            DEBUG_WRITE(AckID);
            DEBUG_WRITE(", DecryptedMessage ");
            DEBUG_WRITEHEXVAL(DecryptedMessage, 8);
            DEBUG_WRITE("\n");

            //if no message then fail
            if(!DecryptedMessage)
                return;

            //confirm it is the disconnect message
            if(*(unsigned int *)DecryptedMessage != DISCONNECT_CMD)
            {
                free(DecryptedMessage);
                return;
            }

            //if an ack is required then send it
            if(AckID)
            {
                //set the ack ID to respond to
                AckID = KnownDevice->ID_In - 1;

                free(DecryptedMessage);

                //send an ack for this message
                DecryptedMessage = this->EncryptPacket(KnownDevice, (uint8_t *)&AckID, sizeof(AckID), &DecryptedMessageLen);
                this->SendPayload(MSG_DisconnectAck, KnownDevice->MAC, DecryptedMessage, DecryptedMessageLen);
            }

            //alert the callback
            if(this->ConnectedCallback)
                this->ConnectedCallback(KnownDevice->MAC, 0, -1);

            //free the message
            free(DecryptedMessage);

            DEBUG_WRITE("Disconnecting and deleting ");
            DEBUG_WRITEMAC(KnownDevice->MAC);
            DEBUG_WRITE("\n");

            //delete the entry for this from our storage and delete it from the array
            this->DeletePref(KnownDevice->MAC);
            this->RemoveKnownDevice(KnownDevice);
            KnownDevice = 0;    //RemoveKnownDevice free'd it
            break;

        case MSG_DisconnectAck:
            //if a broadcast message then ignore
            if(BroadcastMsg)
                return;

            //see if we know of the device
            NewDevice = 0;
            KnownDevice = this->FindKnownDevice(WifiHeader->MAC_Sender);
            if(!KnownDevice)
                return;

            //decrypt the message
            DecryptedMessage = this->DecryptPacket(KnownDevice, Payload, PayloadLen, &DecryptedMessageLen, &AckID);
            if(!DecryptedMessage)
                return;

            //see if the ack is for the ID we expect
            if(*(unsigned int *)DecryptedMessage == KnownDevice->ID_Out - 1)
            {
                DEBUG_WRITE("Disconnect ack for ");
                DEBUG_WRITEMAC(KnownDevice->MAC);
                DEBUG_WRITE("\n");

                //alert the callback
                if(this->ConnectedCallback)
                    this->ConnectedCallback(KnownDevice->MAC, 0, -1);

                //delete the entry for this from our storage and delete it from the array
                this->DeletePref(KnownDevice->MAC);
                this->RemoveKnownDevice(KnownDevice);
                KnownDevice = 0;    //RemoveKnownDevice free'd it
            }

            //free the buffer
            free(DecryptedMessage);
            break;

        default:
            return; //unknown
    };
    DEBUG_WRITELN("Finished processing message");
}

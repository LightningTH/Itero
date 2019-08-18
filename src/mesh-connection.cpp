#include <stdio.h>
#include <string.h>
#include "mesh_internal.h"

int MeshNetworkInternal::Connect(const uint8_t *MAC)
{
    KnownDeviceStruct *NewDevice;
    DHHandshakeStruct DHChal;
    uint8_t Payload[sizeof(DHHandshakeStruct)];

    //if not initialized fail
    if(!this->Initialized)
        return -1;

    //we wish to establish a connection to a mac, first make sure it isn't in our list, if it is just say true
    NewDevice = this->FindKnownDevice(MAC);
    if(NewDevice)
    {
        //if we are not in reset then return
        if(NewDevice->ConnectState != CS_Reset)
            return 1;

        //indicate it is a reset
        DHChal.Challenge = RESET_CMD;
        DHChal.Mask = RESET_CMD;
        NewDevice->ConnectState = CS_ResetConnecting;

        DEBUG_WRITE("LFSR_Reset: ");
        DEBUG_WRITEHEXVAL(NewDevice->LFSR_Reset, 8);
        DEBUG_WRITE(", Mask: ");
        DEBUG_WRITEHEXVAL(NewDevice->LFSR_ResetMask, 8);
        DEBUG_WRITE("\n");

        //setup LFSR_In as our temp storage for Reset to stay in sync with the back and forth comms
        NewDevice->LFSR_In = NewDevice->LFSR_Reset;

        //encrypt it
        this->Encrypt(&DHChal, Payload, sizeof(DHHandshakeStruct), &NewDevice->LFSR_In, NewDevice->LFSR_ResetMask);

        //generate a payload indicating we are attempting to reconnect and send it
        return this->SendPayload(MSG_ConnectRequest, MAC, Payload, sizeof(DHHandshakeStruct));
    }
    else
    {
        //we need to create a new entry and handshake over the encryption between both sides
        NewDevice = (KnownDeviceStruct *)malloc(sizeof(KnownDeviceStruct));
        if(!NewDevice)
            return -1;

        //wipe out the memory
        memset(NewDevice, 0, sizeof(KnownDeviceStruct));

        //setup basic values
        memcpy(NewDevice->MAC, MAC, MAC_SIZE);

        //generate a private value and value to send along for diffie hellman
        NewDevice->LFSR_Reset = DHCreateChallenge(&DHChal.Challenge);
        DHChal.Mask = this->CreateLFSRMask();
        NewDevice->LFSR_ResetMask = DHChal.Mask;
        NewDevice->ConnectState = ConnectStateEnum::CS_Connecting;

        //add to our list
        this->InsertKnownDevice(NewDevice);

        //generate a payload indicating we are attempting to connect and send it
        return this->SendPayload(MSG_ConnectRequest, MAC, (uint8_t *)&DHChal, sizeof(DHChal));
    }
}


//this function is triggered when we are the receiver of MSG_ConnectRequest
int MeshNetworkInternal::ConnectRequest(const uint8_t *MAC, uint8_t *Payload, int PayloadLen)
{
    KnownDeviceStruct *Device;
    DHHandshakeStruct *DHChal;
    DHFinalizeHandshakeStruct DHFinal;
    unsigned int MasterLFSR;
    unsigned int MasterLFSRMask;

    DHChal = (DHHandshakeStruct *)Payload;

    //find our entry
    Device = this->FindKnownDevice(MAC);

    //if we have an entry and it's still in connecting mode then delete and start over
    if(Device && (Device->ConnectState == ConnectStateEnum::CS_Connecting))
    {
        this->RemoveKnownDevice(Device);
        Device = 0;
    }

    //if we have an entry then we have been connected and a reset was kicked off
    if(Device)
    {
        DEBUG_WRITE("Payload len ");
        DEBUG_WRITE(PayloadLen);
        DEBUG_WRITE(" == ");
        DEBUG_WRITE(sizeof(DHHandshakeStruct));
        DEBUG_WRITE("\n");

        if(PayloadLen != sizeof(DHHandshakeStruct))
            return -1;

        //we already have established a connection so they must want to reset everything
        MasterLFSR = Device->LFSR_Reset;
        MasterLFSRMask = Device->LFSR_ResetMask;

        DEBUG_WRITE("LFSR_Reset: ");
        DEBUG_WRITEHEXVAL(Device->LFSR_Reset, 8);
        DEBUG_WRITE(", Mask: ");
        DEBUG_WRITEHEXVAL(Device->LFSR_ResetMask, 8);
        DEBUG_WRITE("\n");

        //attempt to decrypt and see if it matches the reset command
        this->Decrypt(Payload, DHChal, sizeof(DHHandshakeStruct), &MasterLFSR, MasterLFSRMask);
        DEBUG_WRITE("Reset cmd: ");
        DEBUG_WRITEHEXVAL(DHChal->Challenge, 8);
        DEBUG_WRITE(" == ");
        DEBUG_WRITEHEXVAL(RESET_CMD, 8);
        DEBUG_WRITE("\n");

        //if not the reset packet then abort
        if((DHChal->Challenge != RESET_CMD) || (DHChal->Mask != RESET_CMD))
            return -1;

        //reset is good, data was decrypted, reset the ID numbers and setup reset
        Device->ID_In = 0;
        Device->ID_Out = 0;
        Device->ConnectState = ConnectStateEnum::CS_ResetConnecting;
        DHFinal.Chal = 0;
    }
    else
    {
        //finish the handshake for connection
        if(PayloadLen != sizeof(DHHandshakeStruct))
            return -1;

        //we need to create a new entry and handshake over the encryption between both sides
        Device = (KnownDeviceStruct *)malloc(sizeof(KnownDeviceStruct));
        if(!Device)
            return -1;

        //wipe out the memory
        memset(Device, 0, sizeof(KnownDeviceStruct));

        //setup basic values
        memcpy(Device->MAC, MAC, MAC_SIZE);

        //add to our list
        this->InsertKnownDevice(Device);

        //generate our challenge side
        MasterLFSR = DHCreateChallenge(&DHFinal.Chal);

        //we have our private side and the value from the other side, calculate a master
        //value then random generate the other pieces to pass back encrypted with the new information
        //this master value is only used once to get the data sent
        MasterLFSR = DHFinishChallenge(MasterLFSR, DHChal->Challenge);
        MasterLFSRMask = DHChal->Mask;

        //indicate in a connecting state
        Device->ConnectState = ConnectStateEnum::CS_Connecting;
    }
    
    //generate new LFSR values
    for(int i = 0; i < 3; i++)
    {
        DHFinal.LFSR.LFSR[i] = esp_random();
        DHFinal.LFSR.LFSRMask[i] = this->CreateLFSRMask();
    }

    //if this is a reset packet then set the challenge value as the CRC of the LFSR data
    //and encrypt it
    if(Device->ConnectState == ConnectStateEnum::CS_ResetConnecting)
    {
        //change LFSR[0] and it's mask to our reset values as we don't want reset to change
        DHFinal.LFSR.LFSR[0] = Device->LFSR_Reset;
        DHFinal.LFSR.LFSRMask[0] = Device->LFSR_ResetMask;
        
        DHFinal.Chal = this->CalculateCRC(&DHFinal.LFSR, sizeof(DHFinal.LFSR));
        this->Encrypt(&DHFinal.Chal, &DHFinal.Chal, sizeof(DHFinal.Chal), &MasterLFSR, MasterLFSRMask);
    }
    else
    {
        //only set these if we are not resetting
        Device->LFSR_Reset = DHFinal.LFSR.LFSR[0];
        Device->LFSR_ResetMask = DHFinal.LFSR.LFSRMask[0];
    }
    
    //lfsr
    Device->LFSR_In = DHFinal.LFSR.LFSR[1];
    Device->LFSR_Out = DHFinal.LFSR.LFSR[2];

    //masks
    Device->LFSR_InMask = DHFinal.LFSR.LFSRMask[1];
    Device->LFSR_OutMask = DHFinal.LFSR.LFSRMask[2];

    DEBUG_WRITE("LFSR_In: ");
    DEBUG_WRITEHEXVAL(Device->LFSR_In, 8);
    DEBUG_WRITE(", Mask: ");
    DEBUG_WRITEHEXVAL(Device->LFSR_InMask, 8);
    DEBUG_WRITE("\n");

    DEBUG_WRITE("LFSR_Out: ");
    DEBUG_WRITEHEXVAL(Device->LFSR_Out, 8);
    DEBUG_WRITE(", Mask: ");
    DEBUG_WRITEHEXVAL(Device->LFSR_OutMask, 8);
    DEBUG_WRITE("\n");

    DEBUG_WRITE("ConnectState: ");
    DEBUG_WRITE(Device->ConnectState);
    DEBUG_WRITE("\n");
    DEBUG_WRITE("MasterLFSR: ");
    DEBUG_WRITEHEXVAL(MasterLFSR, 8);
    DEBUG_WRITE(", Mask: ");
    DEBUG_WRITEHEXVAL(MasterLFSRMask, 8);
    DEBUG_WRITE("\n");

    //copy our name in
    memset(DHFinal.Name, 0, sizeof(DHFinal.Name));
    memcpy(DHFinal.Name, this->PingData, this->PingDataLen);

    //now encrypt the data and return it
    //note, on a reset MasterLFSR was modified from a previous decryption and the other side
    //is holding that new value for this packet that will be sent
    this->Encrypt(&DHFinal.LFSR, &DHFinal.LFSR, sizeof(DHFinal.LFSR), &MasterLFSR, MasterLFSRMask);
    return this->SendPayload(MSG_ConnHandshake, MAC, (uint8_t *)&DHFinal, sizeof(DHFinal));
}

//this function is triggered when we are the receiver of MSG_ConnHandshake
int MeshNetworkInternal::ConnectHandshake(const uint8_t *MAC, uint8_t *Payload, int PayloadLen)
{
    KnownDeviceStruct *Device;
    DHFinalizeHandshakeStruct *DHFinal;
    unsigned int MasterLFSR;
    unsigned int MasterLFSRMask;
    int Ret;
    ConnectedStruct ConnectedData;

    //find our entry
    Device = this->FindKnownDevice(MAC);

    //if we don't have an entry or it's not in initializing position then fail
    if(!Device || (Device->ConnectState == ConnectStateEnum::CS_Connected) ||
        (Device->ConnectState == ConnectStateEnum::CS_Reset))
        return -1;

    //we tried to establish a connection, we should have an encrypted packet now with values to use
    //get the other side of DH and generate a master key then decrypt the data

    //get the diffie hellman value sent to us
    DHFinal = (DHFinalizeHandshakeStruct *)Payload;

    //we have our private side and the value from the other side, calculate a master
    if(Device->ConnectState == ConnectStateEnum::CS_ResetConnecting)
    {
        MasterLFSR = Device->LFSR_In;   //get the value we stopped at
        MasterLFSRMask = Device->LFSR_ResetMask;

        //in reset, decrypt the CRC Value
        this->Decrypt(&DHFinal->Chal, &DHFinal->Chal, sizeof(DHFinal->Chal), &MasterLFSR, MasterLFSRMask);
    }
    else
    {
        MasterLFSR = DHFinishChallenge(Device->LFSR_Reset, DHFinal->Chal);
        MasterLFSRMask = Device->LFSR_ResetMask;
    }

    DEBUG_WRITE("ConnectState: ");
    DEBUG_WRITE(Device->ConnectState);
    DEBUG_WRITE("\n");
    DEBUG_WRITE("MasterLFSR: ");
    DEBUG_WRITEHEXVAL(MasterLFSR, 8);
    DEBUG_WRITE(", Mask: ");
    DEBUG_WRITEHEXVAL(MasterLFSRMask, 8);
    DEBUG_WRITE("\n");

    //attempt to decrypt the payload
    this->Decrypt(&DHFinal->LFSR, &DHFinal->LFSR, sizeof(DHFinal->LFSR), &MasterLFSR, MasterLFSRMask);

    //if part of the reset then validate the CRC
    if(Device->ConnectState == ConnectStateEnum::CS_ResetConnecting)
    {
        if(DHFinal->Chal != this->CalculateCRC(&DHFinal->LFSR, sizeof(DHFinal->LFSR)))
            return -1;
    }
    else
    {
        //set the LFSR_Reset to new values as this is the first connection
        Device->LFSR_Reset = DHFinal->LFSR.LFSR[0];
        Device->LFSR_ResetMask = DHFinal->LFSR.LFSRMask[0];
    }

    //fill in our side of the values provided

    //NOTE: In and Out are swapped from the other side

    //lfsr
    Device->LFSR_In = DHFinal->LFSR.LFSR[2];
    Device->LFSR_Out = DHFinal->LFSR.LFSR[1];

    //masks
    Device->LFSR_InMask = DHFinal->LFSR.LFSRMask[2];
    Device->LFSR_OutMask = DHFinal->LFSR.LFSRMask[1];

    DEBUG_WRITE("LFSR_In: ");
    DEBUG_WRITEHEXVAL(Device->LFSR_In, 8);
    DEBUG_WRITE(", Mask: ");
    DEBUG_WRITEHEXVAL(Device->LFSR_InMask, 8);
    DEBUG_WRITE("\n");

    DEBUG_WRITE("LFSR_Out: ");
    DEBUG_WRITEHEXVAL(Device->LFSR_Out, 8);
    DEBUG_WRITE(", Mask: ");
    DEBUG_WRITEHEXVAL(Device->LFSR_OutMask, 8);
    DEBUG_WRITE("\n");

    //make sure our IDs are 0
    Device->ID_In = 0;
    Device->ID_Out = 0;

    //both sides should be in sync now, send a connected message
    ConnectedData.ID = CONNECTED_CMD;
    ConnectedData.LFSR = Device->LFSR_Reset;
    ConnectedData.LFSRMask = Device->LFSR_ResetMask;

    //copy our name in
    memset(ConnectedData.Name, 0, sizeof(ConnectedData.Name));
    memcpy(ConnectedData.Name, this->PingData, this->PingDataLen);

    this->Encrypt(&ConnectedData, &ConnectedData, sizeof(ConnectedData), &Device->LFSR_Out, Device->LFSR_OutMask);
    Ret = this->SendPayload(MSG_Connected, MAC, (uint8_t *)&ConnectedData, sizeof(ConnectedData));
    DEBUG_WRITE("SendPayload ret: ");
    DEBUG_WRITE(Ret);
    DEBUG_WRITE("\n");

    if(!Ret)
    {
        //all good, move to connected and trigger the callback if not in a reset
        if((Device->ConnectState != ConnectStateEnum::CS_ResetConnecting) && this->ConnectedCallback)
            this->ConnectedCallback(MAC, DHFinal->Name, 1);

        //if in reset then modify the output slightly if we have any data packet waiting
        if((Device->ConnectState == ConnectStateEnum::CS_ResetConnecting) && Device->LastOutMessage)
        {
            //in reset with a packet, modify our out info slightly so we sync properly
            Device->LFSR_OutPrev = Device->LFSR_Out;
            Device->ID_Out = 1;
            Device->LastOutMessageCheck = 0;
        }

        //change our connection status
        Device->ConnectState = ConnectStateEnum::CS_Connected;

        //store off the entry
        PrefConnStruct NewConnData;
        memcpy(NewConnData.MAC, Device->MAC, MAC_SIZE);
        NewConnData.LFSR_Reset = Device->LFSR_Reset;
        NewConnData.LFSR_ResetMask = Device->LFSR_ResetMask;

        uint8_t DeviceCount;
        uint8_t PrefID;

        //get our ID to update
        PrefID = this->FindPrefID(Device->MAC, &DeviceCount);

        this->prefs->begin("mesh");
        this->prefs->putBytes(String(PrefID).c_str(), (void *)&NewConnData, sizeof(PrefConnStruct));
        if(PrefID == DeviceCount)
            this->prefs->putUChar("count", DeviceCount + 1);
        this->prefs->end();

        DEBUG_WRITE("Writing pref entry ");
        DEBUG_WRITE(PrefID);
        DEBUG_WRITE("\n");
    }
    else
    {
        //something broke, delete the device
        this->DeletePref(Device->MAC);
        this->RemoveKnownDevice(Device);
        if(this->ConnectedCallback)
            this->ConnectedCallback(MAC, 0, 0);
    }

    return Ret;
}

//this function is triggered when we are the receiver of MSG_Connected
int MeshNetworkInternal::Connected(const uint8_t *MAC, uint8_t *Payload, int PayloadLen)
{
    KnownDeviceStruct *Device;
    ConnectedStruct *ConnValue;

    //find our entry
    Device = this->FindKnownDevice(MAC);

    //if we don't have an entry or it's not in initializing position then fail
    if(!Device || (Device->ConnectState == ConnectStateEnum::CS_Connected))
        return -1;

    //payload was not cycled through the LFSR yet so decrypt and check it's value
    ConnValue = (ConnectedStruct *)Payload;
    this->Decrypt(ConnValue, ConnValue, PayloadLen, &Device->LFSR_In, Device->LFSR_InMask);
    if(ConnValue->ID != CONNECTED_CMD)
    {
        //no match, delete the device and fail
        DEBUG_WRITELN("MeshNetwork::Connected: removing known\n");
        this->RemoveKnownDevice(Device);
        DEBUG_WRITELN("MeshNetwork::Connected: known deleted\n");
        return -1;
    }

    DEBUG_WRITELN("MeshNetwork::Connected: data decrypted");

    //grab the reset vectors as everything is good now
    Device->LFSR_Reset = ConnValue->LFSR;
    Device->LFSR_ResetMask = ConnValue->LFSRMask;

    //alert our side that someone established a connection if not a reset
    if((Device->ConnectState != ConnectStateEnum::CS_ResetConnecting) && this->ConnectedCallback)
        this->ConnectedCallback(MAC, ConnValue->Name, 1);

    //store off the entry
    PrefConnStruct NewConnData;
    memcpy(NewConnData.MAC, Device->MAC, MAC_SIZE);
    NewConnData.LFSR_Reset = Device->LFSR_Reset;
    NewConnData.LFSR_ResetMask = Device->LFSR_ResetMask;
    
    uint8_t DeviceCount;
    uint8_t PrefID;

    //get our ID to update
    PrefID = this->FindPrefID(Device->MAC, &DeviceCount);

    this->prefs->begin("mesh");
    this->prefs->putBytes(String(PrefID).c_str(), (void *)&NewConnData, sizeof(PrefConnStruct));
    if(PrefID == DeviceCount)
        this->prefs->putUChar("count", DeviceCount + 1);
    this->prefs->end();

    //if in reset then modify the output slightly if we have any data packet waiting
    if((Device->ConnectState == ConnectStateEnum::CS_ResetConnecting) && Device->LastOutMessage)
    {
        //in reset with a packet, modify our out info slightly so we sync properly
        Device->LFSR_OutPrev = Device->LFSR_Out;
        Device->ID_Out = 1;
        Device->LastOutMessageCheck = 0;
    }

    //all good
    Device->ConnectState = ConnectStateEnum::CS_Connected;
    return 0;
}

int MeshNetworkInternal::Disconnect(const uint8_t MAC[MAC_SIZE])
{
    unsigned int DisconnectMsg = DISCONNECT_CMD;
    uint8_t *EncData;
    unsigned short EncLen;
    KnownDeviceStruct *Device;
    int Ret;

    //find our entry
    Device = this->FindKnownDevice(MAC);

    //if we don't have an entry or it's not in initializing position then fail
    if(!Device)
        return MeshWriteErrors::DeviceDoesNotExist;

    DEBUG_WRITE("Disconnect device state: ");
    DEBUG_WRITE(Device->ConnectState);
    DEBUG_WRITE(", LastOut: ");
    DEBUG_WRITEHEXVAL((unsigned int)Device->LastOutMessage, 8);
    DEBUG_WRITE("\n");

    //if not connected then fail
    if(Device->ConnectState != ConnectStateEnum::CS_Connected)
    {
        this->Connect(Device->MAC);
        return MeshWriteErrors::ResettingConnection;
    }

    //if still buffered data then fail
    if(Device->LastOutMessage)
        return MeshWriteErrors::PreviousWriteNotComplete;

    //encrypt the packet
    EncData = this->EncryptPacket(Device, (uint8_t *)&DisconnectMsg, sizeof(DisconnectMsg), &EncLen);

    //if no valid data then fail
    if(!EncData)
        return MeshWriteErrors::DataTooLarge;

    //device is connected, send a message saying we want to disconnect
    Ret = this->SendPayload(MSG_Disconnect, MAC, EncData, EncLen);
    free(EncData);
    return Ret;
}

int MeshNetworkInternal::ForceDisconnect(const uint8_t MAC[MAC_SIZE])
{
    KnownDeviceStruct *Device;

    //find our entry
    Device = this->FindKnownDevice(MAC);

    //if we don't have an entry or it's not in initializing position then fail
    if(!Device)
        return MeshWriteErrors::DeviceDoesNotExist;


    DEBUG_WRITE("Force disconnecting and deleting ");
    DEBUG_WRITEMAC(Device->MAC);
    DEBUG_WRITE("\n");

    //remove it, if we have a callback report it is deleted
    this->RemoveKnownDevice(Device);
    if(this->ConnectedCallback)
        this->ConnectedCallback(MAC, 0, -1);

    return 0;
}
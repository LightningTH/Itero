#include <Arduino.h>
#include "mesh_internal.h"
#include "mesh.h"
#include <esp_wifi.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

extern "C" {
#include "crypto/base64.h"
}

MeshNetworkInternal *_GlobalMesh = 0;

MeshNetwork *NewMeshNetwork(MeshNetwork::MeshNetworkData *InitData, MeshNetwork::MeshInitErrors *Initialized)
{
    MeshNetworkInternal *ret = new MeshNetworkInternal(InitData, Initialized);
    return ret;
}

MeshNetworkInternal::MeshNetworkInternal(MeshNetworkData *InitData, MeshInitErrors *Initialized)
{
    int Ret;

    this->Initialized = 0;
    this->PingData = 0;
    this->PingDataLen = 0;
    this->MeshMessageBegin = 0;
    this->MeshMessageTail = 0;
    
    //check pointers passed in
    if(!Initialized)
        return;
    else
        *Initialized = MeshInitErrors::UnknownError;

    if(!InitData)
        return;

    //if global is already setup then fail
    if(_GlobalMesh)
    {
        *Initialized = MeshInitErrors::AlreadyInitialized;
        return;
    }

    //if the wifi is not already configured then fail otherwise get the mac address
    if(esp_wifi_get_mac(WIFI_IF_STA, this->MAC) != ESP_OK)
    {
        *Initialized = MeshInitErrors::FailedToGetMac;
        return;
    }

    //setup diffie hellman
    Ret = this->DHInit(InitData->DiffieHellman_P, InitData->DiffieHellman_G);
    if(Ret)
    {
        *Initialized = MeshInitErrors::FailedDiffieHelmanInit;
        return;
    }

    //setup broadcast LFSR
    Ret = this->SetBroadcastLFSR(InitData->BroadcastLFSR, InitData->BroadcastMask1, InitData->BroadcastMask2);
    if(Ret)
    {
        *Initialized = MeshInitErrors::FailedBroadcastLFSRInit;
        return;
    }

    //set default params
    memset(this->KnownDeviceTable, 0, sizeof(this->KnownDeviceTable));
    memset(this->UnknownDeviceTable, 0, sizeof(this->UnknownDeviceTable));

    //setup callbacks
    this->ReceiveMessageCallback = InitData->ReceiveMessageCallback;
    this->BroadcastMessageCallback = InitData->BroadcastMessageCallback;
    this->ConnectedCallback = InitData->ConnectedCallback;
    this->SendFailedCallback = InitData->SendFailedCallback;
    this->PingCallback = InitData->PingCallback;
    this->SendMessageCallback = InitData->SendMessageCallback;

    //setup our global info
    this->MessageWasSent = 0;
    this->BroadcastFlag = InitData->BroadcastFlag;
    _GlobalMesh = this;

    //setup the wifi to watch for the messages we want
    wifi_promiscuous_filter_t filter;
    filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
    esp_wifi_set_promiscuous_rx_cb(Promiscuous_RX);
    esp_wifi_set_promiscuous_filter(&filter);
    if(esp_wifi_set_promiscuous(true) != ESP_OK)
    {
        *Initialized = MeshInitErrors::FailedToEnablePromiscuous;
        return;
    }

    //setup our flash storage
    this->prefs = new Preferences();
    this->ReloadConnections();

    //setup our thread that will re-send messages that haven't been ack'data
    if(pthread_create(&this->MessageCheckThread, NULL, Static_ResendMessages, 0))
    {
        //failed
        *Initialized = MeshInitErrors::FailedThreadInit;
        _GlobalMesh = 0;
        return;
    }

    //create the thread for message processing
    if(pthread_create(&this->MessageRXThread, NULL, Static_ProcessRXMessages, 0))
    {
        //failed
        *Initialized = MeshInitErrors::FailedThreadInit;
        _GlobalMesh = 0;
        return;
    }

    //done
    this->Initialized = 1;
    *Initialized = MeshInitErrors::MeshInitialized;
    return;
}

const uint8_t *MeshNetworkInternal::GetMAC()
{
    return this->MAC;
}

void MeshNetworkInternal::ResetConnectionData()
{
    //wipe out all connection data
    this->prefs->begin("mesh");
    this->prefs->clear();
    this->prefs->end();
}

void MeshNetworkInternal::ReloadConnections()
{
    //reload any connections we had
    uint8_t ConnCount;
    uint8_t CurConn;
    String ConnName;
    PrefConnStruct ConnData;
    KnownDeviceStruct *NewDevice;

    this->prefs->begin("mesh");
    ConnCount = this->prefs->getUChar("count", 0);

    DEBUG_WRITE("Found ");
    DEBUG_WRITE(ConnCount);
    DEBUG_WRITE(" known connections\n");

    for(CurConn = 0; CurConn < ConnCount; CurConn++)
    {
        //grab a connection entry and load it
        this->prefs->getBytes(String(CurConn).c_str(), (void *)&ConnData, sizeof(PrefConnStruct));

        //allocate a new entry and fill it in
        NewDevice = (KnownDeviceStruct *)malloc(sizeof(KnownDeviceStruct));
        memset(NewDevice, 0, sizeof(KnownDeviceStruct));
        memcpy(NewDevice->MAC, ConnData.MAC, MAC_SIZE);
        NewDevice->LFSR_Reset = ConnData.LFSR_Reset;
        
        //indicate it needs to be reconnected
        NewDevice->ConnectState = CS_Reset;
        this->InsertKnownDevice(NewDevice);

        DEBUG_WRITE("Loaded ");
        DEBUG_WRITEMAC(NewDevice->MAC);
        DEBUG_WRITE("\n");
    }

    this->BroadcastMsgID = this->prefs->getUInt("broadcastid", 0);
    this->prefs->end();
}

void MeshNetworkInternal::SetPingData(const uint8_t *Data, uint16_t Len)
{
    //if we had ping data then free it
    if(this->PingData)
        free(this->PingData);

    //allocate a buffer and copy the data over
    this->PingData = (uint8_t *)malloc(Len);
    memcpy(this->PingData, Data, Len);
    this->PingDataLen = Len;
}

void MeshNetworkInternal::SetBroadcastFlag(bool BroadcastFlag)
{
    this->BroadcastFlag = BroadcastFlag;
}

void MeshNetworkInternal::ProcessMessage(const uint8_t *Data, uint16_t Len)
{
    size_t InLen;
    uint8_t *InData;

    InData = base64_decode(Data, Len, &InLen);
    if(!InData)
        return;

    //call our internal function as if the data showed up normally
    this->PromiscuousRX((void *)InData, WIFI_PKT_MGMT);

    free(InData);
}

bool MeshNetworkInternal::CanBroadcast()
{
    return this->BroadcastFlag;
}
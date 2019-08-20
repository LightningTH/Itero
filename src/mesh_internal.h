#ifndef _MESH_INTERNAL_H
#define _MESH_INTERNAL_H

#include "mesh.h"
#include "debug.h"

#include <stdint.h>
#include <esp_wifi.h>
#include <pthread.h>
#include <Preferences.h>

#define TABLE_SIZE 8
#define TABLE_MASK (TABLE_SIZE - 1)
#define MAX_PACKET_SIZE 1000

#define field_sizeof(t, f) (sizeof(((t*)0)->f))

//these values are just random generated
#define RESET_CMD 0xa19f0c21
#define CONNECTED_CMD 0x229c0985
#define DISCONNECT_CMD 0x8f223a7b
#define VALID_PACKET_ID 0x9056acd2

void Promiscuous_RX(void *buf, wifi_promiscuous_pkt_type_t type);
void *Static_ResendMessages(void *);
void *Static_ProcessRXMessages(void *);

typedef class MeshNetworkInternal : public MeshNetwork
{
    public:
        //Mesh network initialization
        MeshNetworkInternal(MeshNetworkData *InitData, MeshInitErrors *Initialized);

        //write data to a specific mac on the mesh network, returns the length written
        int Write(const uint8_t MAC[MAC_SIZE], const uint8_t *Data, unsigned short DataLen);

        //establish a connection a device, this only needs to be done once per device we talk to
        //ConnectedCallback will be triggered with the mac once a connection is established
        //or when a requested connection fails
        int Connect(const uint8_t MAC[MAC_SIZE]);

        //disconnect from a known connection
        int Disconnect(const uint8_t MAC[MAC_SIZE]);

        //force a disconnection without alerting the other side
        int ForceDisconnect(const uint8_t MAC[MAC_SIZE]);;

        //see if a device is known
        int IsDeviceKnown(const uint8_t MAC[MAC_SIZE]);

        //get mac address of local device
        const uint8_t *GetMAC();

        //send out a ping to see who is near by
        void Ping();

        //reset connection data stored on the device
        void ResetConnectionData();

        //promiscuous function
        void PromiscuousRX(void *buf, wifi_promiscuous_pkt_type_t type);

        //check message and resend function
        void ResendMessages();
        void ProcessRXMessages();

        //Fill an array with connected mac values, up to the size of the buffer
        //return the number of MACs known if BufferSize is 0 otherwise return
        //number of MACs put into the buffer
        int GetConnectedDevices(uint8_t *MACBuffer, int BufferSize);

        //specify the ping data to send
        void SetPingData(const uint8_t *Data, uint16_t Len);

    private:
        //we are using a similar but not identical header frame for 802.11
        //namely we removed the BSS ID and extended SequenceID to be 4 bytes
        typedef struct __attribute__((packed)) WifiHeaderStruct
        {
            unsigned short FC;
            unsigned short Duration;
            uint8_t MAC_Reciever[6];
            uint8_t MAC_Sender[6];
            uint8_t Type;
            uint8_t MAC_Unused[5];
            uint16_t SequenceControl;
        } WifiHeaderStruct;

        //if this list is greater than 0x67 then modify PromiscuousRX action mask
        typedef enum MessageTypeEnum
        {
            MSG_ConnectRequest = 0x60,
            MSG_ConnHandshake = 0x61,
            MSG_Connected = 0x62,
            MSG_Message = 0x63,
            MSG_MessageAck = 0x64,
            MSG_Ping = 0x65,
            MSG_PingAck = 0x66,
            MSG_Disconnect = 0x67,
            MSG_DisconnectAck = 0x68
        } MessageTypeEnum;

        typedef enum ConnectStateEnum
        {
            CS_Connected,
            CS_Connecting,
            CS_Reset,
            CS_ResetConnecting
        } ConnectStateEnum;

        //bit masks are in blocks of 5 bits allowing for up to 6 bits to be used
        //as part of the LFSR calculation, must be even
        typedef struct KnownDeviceStruct
        {
            uint8_t MAC[6];
            unsigned int LFSR_Reset;            //LFSR reset value
            unsigned int LFSR_ResetMask;        //bit mask for LFSR Reset
            unsigned int LFSR_In;               //LFSR for incoming data
            unsigned int LFSR_InPrev;           //previous LFSR for incoming data
            unsigned int LFSR_InMask;           //bit mask for LFSR In
            unsigned int LFSR_Out;              //LFSR for outgoing data
            unsigned int LFSR_OutPrev;          //LFSR for outgoing data
            unsigned int LFSR_OutMask;          //bit mask for LFSR Out
            unsigned int ID_In;                 //Incrementing ID for incoming
            unsigned int ID_Out;                //Incrementing ID for outgoing
            ConnectStateEnum ConnectState;      //indicate if we are connecting
            uint8_t *LastOutMessage;            //last message sent encrypted
            unsigned short LastOutMessageLen;   //length of last message sent
            unsigned short LastOutMessageCheck; //flag indicating how many times we've checked before sending the message
            struct KnownDeviceStruct *Next;
        } KnownDeviceStruct;

        typedef struct __attribute__((packed)) PacketHeaderStruct
        {
            uint8_t InternalCRC;
            unsigned int SequenceID;
        } PacketHeaderStruct;

        typedef struct UnknownDeviceStruct
        {
            uint8_t MAC[MAC_SIZE];
            unsigned int ID;
            struct UnknownDeviceStruct *Next;
        } UnknownDeviceStruct;

        typedef struct __attribute__((packed)) DHHandshakeStruct
        {
            unsigned int Challenge;
            unsigned int Mask;
        } DHHandshakeStruct;

        typedef struct __attribute__((packed)) DHFinalizeHandshakeStruct
        {
            unsigned int Chal;
            struct LFSR
            {
                unsigned int LFSR[3];
                unsigned int LFSRMask[3];
            } LFSR;
            char Name[20];
        } DHFinalizeHandshakeStruct;

        typedef struct __attribute__((packed)) ConnectedStruct
        {
            unsigned int ID;
            unsigned int LFSR;
            unsigned int LFSRMask;
            char Name[20];
        } ConnectedStruct;

        typedef struct __attribute__((packed)) MeshMessageStruct
        {
            MeshMessageStruct *next;
            size_t Len;
            size_t Count;
            uint8_t Message[0];
        } MeshMessageStruct;

        //our mac for this device
        uint8_t MAC[MAC_SIZE];

        //pointers to our entries, using a hash table to identify the entry
        UnknownDeviceStruct *UnknownDeviceTable[TABLE_SIZE];
        KnownDeviceStruct *KnownDeviceTable[TABLE_SIZE];
        MessageCallbackFunc ReceiveMessageCallback;
        MessageCallbackFunc BroadcastMessageCallback;
        MessageCallbackFunc PingCallback;
        ConnectedCallbackFunc ConnectedCallback;
        SendFailedCallbackFunc SendFailedCallback;

        //ping data
        uint8_t *PingData;
        uint16_t PingDataLen;

        //begin/tail pointers for stored messages to be processed
        MeshMessageStruct *MeshMessageBegin;
        MeshMessageStruct *MeshMessageTail;
        pthread_t MessageRXThread;

        //LFSR for broadcast messages
        unsigned int LFSR_Broadcast;
        unsigned int LFSR_BroadcastMask;
        unsigned int BroadcastMsgID;

        int Initialized;
        pthread_t MessageCheckThread;
        int MessageWasSent;                 //flag to indicate that a message was sent so our checking function will process through possible connections

        //internal functions
        void HandleRXMessage(uint8_t *Data, size_t Len, size_t Count);
        
        //init
        int SetBroadcastLFSR(unsigned int BroadcastLFSR, uint8_t Mask[3]);
        int DHInit(unsigned int P, unsigned int G);

        //enryption
        void Encrypt(const void *InData, void *OutData, unsigned short DataLen, unsigned int *LFSR, unsigned int LFSR_Mask);
        void Decrypt(const void *InData, void *OutData, unsigned short DataLen, unsigned int *LFSR, unsigned int LFSR_Mask);
        uint8_t *EncryptPacket(KnownDeviceStruct *Device, const uint8_t *InData, unsigned short DataLen, unsigned short *OutPacketLen);
        uint8_t *DecryptPacket(KnownDeviceStruct *Device, const uint8_t *InPacket, unsigned short PacketLen, unsigned short *OutDataLen, unsigned int *DoAck);
        uint8_t *EncryptBroadcastPacket(const uint8_t *InData, unsigned short DataLen, unsigned short *OutPacketLen);
        uint8_t *DecryptBroadcastPacket(UnknownDeviceStruct *Device, const uint8_t *InPacket, unsigned short PacketLen, unsigned short *OutDataLen);
        uint8_t *EncryptPacketCommon(unsigned int SequenceID, unsigned int *LFSR, unsigned int LFSRMask, const uint8_t *InData, unsigned short DataLen, unsigned short *OutPacketLen);
        uint8_t *DecryptPacketCommon(unsigned int SequenceID, unsigned int *LFSR, unsigned int LFSRMask, const uint8_t *InPacket, unsigned short PacketLen, unsigned short *OutDataLen);

        //lfsr and crc
        unsigned int CreateLFSRMask();
        unsigned int PermuteBroadcastLFSR(const uint8_t *MAC, unsigned int ID);
        unsigned int CalculateLFSR(unsigned int LFSR, unsigned int Mask);
        uint8_t CalculateCRC(const void *Data, unsigned int DataLen);
        uint8_t CalculateCRC(const void *Data, unsigned int DataLen, uint8_t StartCRC);

        //device tracking
        UnknownDeviceStruct *FindUnknownDevice(const uint8_t *HeaderData);
        KnownDeviceStruct *FindKnownDevice(const uint8_t *MAC);
        int InsertUnknownDevice(UnknownDeviceStruct *NewDevice);
        int InsertKnownDevice(KnownDeviceStruct *NewDevice);
        int RemoveKnownDevice(KnownDeviceStruct *Device);
        int GetKnownDeviceCount();
        
        //connecting functions
        int ConnectRequest(const uint8_t *MAC, uint8_t *Payload, int PayloadLen);
        int ConnectHandshake(const uint8_t *MAC, uint8_t *Payload, int PayloadLen);
        int Connected(const uint8_t *MAC, uint8_t *Payload, int PayloadLen);

        //diffie Hellman
        unsigned int DHPowMod(unsigned int g, unsigned int priv);
        unsigned int DHCreateChallenge(unsigned int *challenge);
        unsigned int DHFinishChallenge(unsigned int priv, unsigned int challenge);
        unsigned int DH_P, DH_G;

        //payload handling code
        int SendPayload(MessageTypeEnum MsgType, const uint8_t *MAC, const void *InData, unsigned short DataLen);

        typedef struct __attribute__((packed)) PrefConnStruct
        {
            uint8_t MAC[6];
            unsigned int LFSR_Reset;            //LFSR reset value
            unsigned int LFSR_ResetMask;        //bit mask for LFSR Reset            
        } PrefConnStruct;

        void ReloadConnections();
        Preferences *prefs;
        uint8_t FindPrefID(const uint8_t *MAC, uint8_t *DeviceCount);
        void DeletePref(const uint8_t *MAC);

} MeshNetworkInternal;

extern MeshNetworkInternal *_GlobalMesh;

#endif
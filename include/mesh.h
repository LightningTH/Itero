#ifndef _MESH_H
#define _MESH_H

#include <stdint.h>
#include <esp_wifi.h>

#define MAC_SIZE 6

typedef void (*MessageCallbackFunc)(const uint8_t *From_MAC, const uint8_t *Data, unsigned int DataLen);
typedef void (*ConnectedCallbackFunc)(const uint8_t *MAC, const char *Name, int Succeeded);
typedef void (*SendFailedCallbackFunc)(const uint8_t *MAC);

typedef class MeshNetwork
{
    public:
        //potential error returns from attempting to initialize the mesh network
        typedef enum MeshInitErrors
        {
            AlreadyInitialized = -6,
            FailedToGetMac,
            FailedDiffieHelmanInit,
            FailedBroadcastLFSRInit,
            FailedToEnablePromiscuous,
            FailedThreadInit,
            UnknownError,
            MeshInitialized = 0
        } MeshInitErrors;

        typedef enum MeshWriteErrors
        {
            OutOfMemory = -6,
            MeshNotInitialized,
            DataTooLarge,
            DeviceDoesNotExist,
            ResettingConnection,
            PreviousWriteNotComplete
        } MeshWriteErrors;

        //Value to use for MAC when broadcasting via Write()
        const uint8_t BroadcastMAC[MAC_SIZE] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
        
        //Required mesh network information during initialization
        typedef struct MeshNetworkData
        {
            unsigned int BroadcastLFSR[2];              //two values that are not 0 or all bits set
            uint8_t BroadcastMask1[3];                  //each value between 1 and 31 inclusive, co-prime against the number 32 (no common divisor for all numbers)
            uint8_t BroadcastMask2[3];                  //each value between 1 and 31 inclusive, co-prime against the number 32 (no common divisor for all numbers)
            unsigned long long DiffieHellman_P;             //must be prime
            unsigned long long  DiffieHellman_G;            //must be non-zero and smaller than P, preferably prime
            MessageCallbackFunc ReceiveMessageCallback;     //function to call when receiving a message from a known/connected device
                                                            //DataSize = 0          - Acknowledgement of direct message sent to a MAC
            MessageCallbackFunc PingCallback;               //function to call when pinged from a device
            MessageCallbackFunc BroadcastMessageCallback;   //function to call when a broadcast message is seen
            ConnectedCallbackFunc ConnectedCallback;        //function to call when a new connection is completed
                                                            //Succeeded = 1         - Connection succeeded
                                                            //Succeeded = 0         - Connection failed
                                                            //Succeeded = -1        - Connection was disconnected
            SendFailedCallbackFunc SendFailedCallback;       //function to call when a direct message fails to be ack'd
        } MeshNetworkData;

        //write data to a specific mac on the mesh network, returns the length written
        //See MeshWriteErrors for potential error values
        virtual int Write(const uint8_t MAC[MAC_SIZE], const uint8_t *Data, unsigned short DataLen);

        //establish a connection a device, this only needs to be done once per device we talk to
        //ConnectedCallback will be triggered with the mac once a connection is established
        //or when a requested connection fails
        //ret values
        //  -1 - error
        //   0 - connecting
        //   1 - already connected in the past
        virtual int Connect(const uint8_t MAC[MAC_SIZE]);

        //disconnect from a known connection
        virtual int Disconnect(const uint8_t MAC[MAC_SIZE]);

        //force a disconnect without alerting the other side
        virtual int ForceDisconnect(const uint8_t MAC[MAC_SIZE]);
        
        //see if a device is known
        virtual int IsDeviceKnown(const uint8_t MAC[MAC_SIZE]);

        //get mac address of local device that is being transmitted on
        virtual const uint8_t *GetMAC();

        //send out a ping to see who is near by
        virtual void Ping();

        //reset connection data stored on the device
        virtual void ResetConnectionData();

        //Fill an array with connected mac values, up to the size of the buffer
        //return the number of MACs known if BufferSize is 0 otherwise return
        //number of MACs put into the buffer
        virtual int GetConnectedDevices(uint8_t *MACBuffer, int BufferSize);

        //specify the ping data to send
        virtual void SetPingData(const uint8_t *Data, uint16_t Len);
} MeshNetwork;

//Mesh network initialization
MeshNetwork *NewMeshNetwork(MeshNetwork::MeshNetworkData *InitData, MeshNetwork::MeshInitErrors *Initialized);

#endif
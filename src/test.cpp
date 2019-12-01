#include <Arduino.h>
#include <esp_wifi.h>
#include "mesh.h"
#include "HardwareSerial.h"

MeshNetwork *Mesh;

char SerialInput[256];
int SerialLen;

void PrintMenu();

void SerialPrintMAC(const uint8_t *mac)
{
    char debug_mac_buffer[19];
    sprintf(debug_mac_buffer, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    Serial.print(debug_mac_buffer);    
}

char *GetSerialData()
{
    int SerialChar;
    int ReturnBuffer;

    ReturnBuffer = 0;
    while(!ReturnBuffer)
    {
        SerialChar = Serial.read();
        if(SerialChar == -1)
        {
            delay(100);
            continue;
        }

        if(SerialChar == 10)
            continue;

        if(SerialChar == 13)
            ReturnBuffer = 1;
        else
        {
            SerialInput[SerialLen++] = SerialChar & 0xff;
            if(SerialLen >= (sizeof(SerialInput) - 1))
                ReturnBuffer = 1;
        }
    }

    //return the data in it
    SerialInput[SerialLen] = 0;
    SerialLen = 0;
    return SerialInput;
}

void MessageReceived(const uint8_t *From_MAC, const uint8_t *Data, unsigned int DataLen)
{
    if(DataLen == 0) {
        Serial.print("Messaged received by ");
        SerialPrintMAC(From_MAC);
        Serial.print("\n");
    } else if(DataLen == 0xffffffff) {
        Serial.print("Ping received from ");
        SerialPrintMAC(From_MAC);
        Serial.print("\n");
    } else {
        Serial.print("Saw message from ");
        SerialPrintMAC(From_MAC);
        Serial.print(": data len ");
        Serial.print(DataLen);
        Serial.print(", data: ");
        Serial.print((char *)Data);
        Serial.print("\n");
    }
}

void BroadcastMessageReceived(const uint8_t *From_MAC, const uint8_t *Data, unsigned int DataLen)
{
    Serial.print("Saw broadcast from ");
    SerialPrintMAC(From_MAC);
    Serial.print(": data len ");
    Serial.print(DataLen);
    Serial.print(", data: ");
    Serial.print((char *)Data);
    Serial.print("\n");
}

void DeviceConnected(const uint8_t *MAC, const char *Name, int Succeeded)
{
    if(Succeeded == -1)
        Serial.print("Disconnection from ");
    else
        Serial.print("Connection to ");

    SerialPrintMAC(MAC);
    Serial.printf(", name %s ", Name);

    if(Succeeded == 1)
        Serial.print(" succeeded\n");
    else if(Succeeded == 0)
        Serial.print(" failed\n");
    else
        Serial.print(" completed\n");
    
}

void SendToDeviceFailed(const uint8_t *MAC)
{
    Serial.print("Failed to send to device ");
    SerialPrintMAC(MAC);
    Serial.print("\n");
}

void setup()
{
    MeshNetwork::MeshNetworkData MeshInitData;
    MeshNetwork::MeshInitErrors MeshInitialized;
    const uint8_t *mac;
    char buffer[19];

    SerialLen = 0;
    Serial.begin(115200);
    Serial.println("Doing setup\n");

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if(esp_wifi_init(&cfg) != ESP_OK)
    {
        Serial.println("Error initializing wifi");
    }

    if(esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK)
        Serial.println("Error setting wifi to STA");

    if(esp_wifi_start() != ESP_OK)
        Serial.println("Error starting wifi in STA mode");

    //initialize the mesh network configuration
    MeshInitData.BroadcastMask1[0] = 13;
    MeshInitData.BroadcastMask1[1] = 8;
    MeshInitData.BroadcastMask1[2] = 21;
    MeshInitData.BroadcastMask2[0] = 23;
    MeshInitData.BroadcastMask2[1] = 18;
    MeshInitData.BroadcastMask2[2] = 30;
    MeshInitData.BroadcastLFSR[0] = 0xf919b1b6;
    MeshInitData.BroadcastLFSR[1] = 0xb1eb535e;
    MeshInitData.DiffieHellman_P = 12412372739946577469ULL;
    MeshInitData.DiffieHellman_G = 11011158976040270681ULL;
    MeshInitData.SendFailedCallback = SendToDeviceFailed;
    MeshInitData.ConnectedCallback = DeviceConnected;
    MeshInitData.ReceiveMessageCallback = MessageReceived;
    MeshInitData.BroadcastMessageCallback = BroadcastMessageReceived;

    Mesh = NewMeshNetwork(&MeshInitData, &MeshInitialized);
    if(!Mesh || (MeshInitialized != MeshNetwork::MeshInitErrors::MeshInitialized))
    {
        Serial.print("Error initializing mesh: ");
        Serial.print(MeshInitialized);
        Serial.print("\n");
        Mesh = 0;   //guarantee not setup
    }
    else
    {
        mac = Mesh->GetMAC();
        sprintf(buffer, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        Serial.print("MAC used by mesh: ");
        Serial.print(buffer);
        Serial.print("\n");
    }

    PrintMenu();
    return;
}

const uint8_t DeviceMacs[2][MAC_SIZE] = {//{0xcc, 0x50, 0xe3, 0xa8, 0x40, 0xd0},
                                         {0xcc, 0x50, 0xe3, 0xa9, 0x9b, 0xb4},
                                         {0xb4, 0xe6, 0x2d, 0xf6, 0xe6, 0xd5}};

int GetOtherDevice()
{
    //get the mac, if it matches the first entry return the 2nd
    if(memcmp(Mesh->GetMAC(), DeviceMacs[0], 6) == 0)
        return 1;

    //return 1st entry
    return 0;
}

void PrintMenu()
{
    Serial.println(
        "Test menu\n"
        "---------\n"
        "1. Send broadcast\n"
        "2. Connect\n"
        "3. Send message to connected device\n"
        "4. Ping\n"
        "5. Reset Connection Data in storage\n"
        "6. Disconnect\n"
    );
}

void loop()
{
    char *SerialCmd;
    int DeviceID;

    SerialCmd = GetSerialData();

    //do something
    switch(SerialCmd[0])
    {
        case 0:
            PrintMenu();
            break;

        case 0x31:
        {
            //send broadcast
            String Msg;
            Msg += "LightningB\x01😈 BROADCAST TESTING WITH EMOJI 😈";
            Serial.printf("Length of emoji line: %d\n", Msg.length());
            Mesh->Write(Mesh->BroadcastMAC, (uint8_t *)Msg.c_str(), Msg.length());
            break;
        }
        case 0x32:
            //connect
            Mesh->Connect(DeviceMacs[GetOtherDevice()]);
            break;

        case 0x33:
            //send message to connected device
            DeviceID = GetOtherDevice();

            Serial.println("Message?");
            SerialCmd = GetSerialData();

            Mesh->Write(DeviceMacs[DeviceID], (uint8_t *)SerialCmd, strlen(SerialCmd) + 1);
            break;

        case 0x34:
            Mesh->Ping();
            break;

        case 0x35:
            Mesh->ResetConnectionData();
            break;

        case 0x36:
            DeviceID = GetOtherDevice();
            Mesh->Disconnect(DeviceMacs[DeviceID]);
            break;

        default:
            Serial.println("Unknown command");
    };

    //print the menu
    PrintMenu();
}

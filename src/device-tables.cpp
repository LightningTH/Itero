#include <stdio.h>
#include <string.h>
#include "mesh_internal.h"
#include "debug.h"

int MeshNetworkInternal::GetConnectedDevices(uint8_t *MACBuffer, int BufferSize)
{
    int Count;
    int CurPos;
    int TablePos;
    KnownDeviceStruct *CurDevice;

    //if no buffer then return the number that exist
    if(BufferSize == 0)
        return this->GetKnownDeviceCount();

    //make sure we have a buffer
    if(!MACBuffer)
        return -1;

    //all good, fill in the buffer up to the maximum size
    CurPos = 0;
    Count = 0;
    for(TablePos = 0; TablePos < TABLE_SIZE; TablePos++)
    {
        CurDevice = this->KnownDeviceTable[TablePos];
        while(CurDevice)
        {
            //if we ran out of space then exit
            if((CurPos + MAC_SIZE) > BufferSize)
                break;

            //add it
            memcpy(&MACBuffer[CurPos], CurDevice->MAC, MAC_SIZE);
            CurPos += MAC_SIZE;
            Count++;

            //next entry
            CurDevice = CurDevice->Next;
        }

        //if we ran out of space then exit
        if((CurPos + MAC_SIZE) > BufferSize)
            break;
    }

    return Count;
}

void MeshNetworkInternal::DeletePref(const uint8_t *MAC)
{
    //cycle through all of the preference entries locating the MAC
    int DeviceCount = this->GetKnownDeviceCount();
    PrefConnStruct ConnData;
    PrefConnStruct NewConnData;
    uint8_t i;

    this->prefs->begin("mesh");
    for(i = 0; i < DeviceCount; i++)
    {
        if(this->prefs->getBytes(String(i).c_str(), (void *)&ConnData, sizeof(PrefConnStruct)) != sizeof(PrefConnStruct))
            break;

        //if mac matches then stop looking
        if(memcmp(ConnData.MAC, MAC, MAC_SIZE) == 0)
            break;
    }

    //if we hit the end stop
    if(i == DeviceCount)
        return;

    //found the entry, if this isn't our last entry then grab the last entry and overwrite it
    if(i != (DeviceCount - 1))
    {
        //get the last entry
        if(this->prefs->getBytes(String(DeviceCount - 1).c_str(), (void *)&NewConnData, sizeof(PrefConnStruct)) != sizeof(PrefConnStruct))
            return;

        //replace our current entry
        this->prefs->putBytes(String(i).c_str(), (void *)&NewConnData, sizeof(PrefConnStruct));
    }

    //adjust the count
    DeviceCount--;
    this->prefs->putUChar("count", DeviceCount);
    this->prefs->end();
}

uint8_t MeshNetworkInternal::FindPrefID(const uint8_t *MAC, uint8_t *DeviceCount)
{
    //cycle through all of the preference entries locating the MAC
    //note, this is called after a new device is already in the list so count will be 1 high
    *DeviceCount = this->GetKnownDeviceCount() - 1;
    PrefConnStruct ConnData;
    uint8_t i;

    this->prefs->begin("mesh");
    for(i = 0; i < *DeviceCount; i++)
    {
        if(this->prefs->getBytes(String(i).c_str(), (void *)&ConnData, sizeof(PrefConnStruct)) != sizeof(PrefConnStruct))
            break;

        //if mac matches then stop looking
        if(memcmp(ConnData.MAC, MAC, MAC_SIZE) == 0)
            break;
    }

    this->prefs->end();

    //return the ID to use
    return i;
}

int MeshNetworkInternal::IsDeviceKnown(const uint8_t MAC[MAC_SIZE])
{
    return (this->FindKnownDevice(MAC) != 0);
}

MeshNetworkInternal::UnknownDeviceStruct *MeshNetworkInternal::FindUnknownDevice(const uint8_t *HeaderData)
{
    UnknownDeviceStruct *CurDevice;

    CurDevice = this->UnknownDeviceTable[this->CalculateCRC(HeaderData, MAC_SIZE) & TABLE_MASK];
    while(CurDevice)
    {
        //check and see if it matches
        if(memcmp(CurDevice->MAC, HeaderData, MAC_SIZE) == 0)
            break;

        //no match, try the next entry
        CurDevice = CurDevice->Next;
    }

    //return what we found or 0 for nothing
    return CurDevice;
}

int MeshNetworkInternal::InsertUnknownDevice(UnknownDeviceStruct *NewDevice)
{
    //insert an entry into the list
    UnknownDeviceStruct *CurDevice;
    unsigned int Index;

    Index = this->CalculateCRC(NewDevice->MAC, MAC_SIZE) & TABLE_MASK;
    CurDevice = this->UnknownDeviceTable[Index];
    while(CurDevice)
    {
        //check and see if it matches, if so then error
        if(memcmp(CurDevice->MAC, NewDevice->MAC, sizeof(UnknownDeviceStruct::MAC)) == 0)
            return -1;

        //no match, try the next entry
        CurDevice = CurDevice->Next;
    }

    //insert into our table as it isn't in the list
    NewDevice->Next = this->UnknownDeviceTable[Index];
    this->UnknownDeviceTable[Index] = NewDevice;

    //return that all is good
    return 0;
}

MeshNetworkInternal::KnownDeviceStruct *MeshNetworkInternal::FindKnownDevice(const uint8_t *MAC)
{
    KnownDeviceStruct *CurDevice;

    DEBUG_WRITE("FindKnownDevice ");
    DEBUG_WRITEMAC(MAC);
    DEBUG_WRITE("\n");

    CurDevice = this->KnownDeviceTable[this->CalculateCRC(MAC, MAC_SIZE) & TABLE_MASK];
    while(CurDevice)
    {
        //check and see if it matches
        if(memcmp(CurDevice->MAC, MAC, MAC_SIZE) == 0)
            break;

        //no match, try the next entry
        CurDevice = CurDevice->Next;
    }

    //return what we found or 0 for nothing
    return CurDevice;
}

int MeshNetworkInternal::GetKnownDeviceCount()
{
    KnownDeviceStruct *CurDevice;
    int i;
    int Count = 0;

    for(i = 0; i < TABLE_SIZE; i++)
    {
        CurDevice = this->KnownDeviceTable[i];
        while(CurDevice)
        {
            Count++;
            CurDevice = CurDevice->Next;
        }
    }

    //return what we found
    return Count;

}

int MeshNetworkInternal::InsertKnownDevice(KnownDeviceStruct *NewDevice)
{
    //insert an entry into the list
    KnownDeviceStruct *CurDevice;
    unsigned int Index;

    Index = this->CalculateCRC(NewDevice->MAC, MAC_SIZE) & TABLE_MASK;
    CurDevice = this->KnownDeviceTable[Index];
    while(CurDevice)
    {
        //check and see if it matches, if so then error
        if(memcmp(CurDevice->MAC, NewDevice->MAC, MAC_SIZE) == 0)
            return -1;

        //no match, try the next entry
        CurDevice = CurDevice->Next;
    }

    //insert into our table as it isn't in the list
    NewDevice->Next = this->KnownDeviceTable[Index];
    this->KnownDeviceTable[Index] = NewDevice;

    //return that all is good
    return 0;
}

int MeshNetworkInternal::RemoveKnownDevice(KnownDeviceStruct *Device)
{
    //remove an entry from the list
    KnownDeviceStruct *CurDevice;
    KnownDeviceStruct *PrevDevice;
    unsigned int Index;

    Index = this->CalculateCRC(Device->MAC, MAC_SIZE) & TABLE_MASK;
    CurDevice = this->KnownDeviceTable[Index];
    PrevDevice = 0;
    while(CurDevice)
    {
        //check and see if it matches
        if(CurDevice == Device)
        {
            if(PrevDevice)
                PrevDevice->Next = CurDevice->Next;
            else
                this->KnownDeviceTable[Index] = CurDevice->Next;
            
            if(CurDevice->LastOutMessage)
                free(CurDevice->LastOutMessage);
            free(CurDevice);
            break;
        }

        //no match, try the next entry
        PrevDevice = CurDevice;
        CurDevice = CurDevice->Next;
    }

    //if current device is 0 then we didn't find it
    if(!CurDevice)
        return -1;

    //all good
    return 0;
}
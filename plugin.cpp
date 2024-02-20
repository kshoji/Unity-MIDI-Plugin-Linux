#include <algorithm>
#include <chrono>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <alsa/asoundlib.h>

typedef void ( *OnSendMessageDelegate )( const char*, const char* ) __attribute__((cdecl));

#ifdef __cplusplus
extern "C" {
#endif

void SetSendMessageCallback(OnSendMessageDelegate callback);

void InitializeMidiLinux();
void TerminateMidiLinux();

const char* GetDeviceNameLinux(const char* deviceId);

void SendMidiNoteOff(const char* deviceId, char channel, char note, char velocity);
void SendMidiNoteOn(const char* deviceId, char channel, char note, char velocity);
void SendMidiPolyphonicAftertouch(const char* deviceId, char channel, char note, char pressure);
void SendMidiControlChange(const char* deviceId, char channel, char func, char value);
void SendMidiProgramChange(const char* deviceId, char channel, char program);
void SendMidiChannelAftertouch(const char* deviceId, char channel, char pressure);
void SendMidiPitchWheel(const char* deviceId, char channel, short amount);
void SendMidiSystemExclusive(const char* deviceId, unsigned char* data, int length);
void SendMidiTimeCodeQuarterFrame(const char* deviceId, char value);
void SendMidiSongPositionPointer(const char* deviceId, short position);
void SendMidiSongSelect(const char* deviceId, char song);
void SendMidiTuneRequest(const char* deviceId);
void SendMidiTimingClock(const char* deviceId);
void SendMidiStart(const char* deviceId);
void SendMidiContinue(const char* deviceId);
void SendMidiStop(const char* deviceId);
void SendMidiActiveSensing(const char* deviceId);
void SendMidiReset(const char* deviceId);

#ifdef __cplusplus
}
#endif

std::map<std::string, snd_rawmidi_t*> midiInputMap;
std::map<std::string, snd_rawmidi_t*> midiOutputMap;
std::map<std::string, snd_seq_addr_t> virtualMidiInputMap;
std::map<std::string, snd_seq_addr_t> virtualMidiOutputMap;
std::map<std::string, std::string> deviceNames;

snd_seq_t *seq_handle = nullptr;
int selfClientId;
int selfPortNumber;

const char *GAME_OBJECT_NAME = "MidiManager";

volatile bool isStopped;

OnSendMessageDelegate onSendMessage;

void UnitySendMessage(const char* obj, const char* method, const char* msg) {
    if (onSendMessage) {
        onSendMessage(method, msg);
    }
}

void virtualMidiEventWatcher() {
    snd_seq_event_t *ev = nullptr;
    char deviceId[32];
    char eventMessage[128];

    while (!isStopped && seq_handle != nullptr) {
        snd_seq_event_input(seq_handle, &ev);

        sprintf(deviceId, "seq:%d-%d", ev->data.addr.client, ev->data.addr.port);
        if (virtualMidiInputMap.find(deviceId) == virtualMidiInputMap.end()) {
            // ignore if not connected
            continue;
        }

        // https://www.alsa-project.org/alsa-doc/alsa-lib/group___seq_events.html#gaef39e1f267006faf7abc91c3cb32ea40
        switch (ev->type) {
            case SND_SEQ_EVENT_NOTEON:
                sprintf(eventMessage, "%s,0,%d,%d,%d", deviceId, ev->data.note.channel, ev->data.note.note, ev->data.note.velocity);
                UnitySendMessage(GAME_OBJECT_NAME, "OnMidiNoteOn", eventMessage);
                break;
            case SND_SEQ_EVENT_NOTEOFF:
                sprintf(eventMessage, "%s,0,%d,%d,%d", deviceId, ev->data.note.channel, ev->data.note.note, ev->data.note.velocity);
                UnitySendMessage(GAME_OBJECT_NAME, "OnMidiNoteOff", eventMessage);
                break;
            case SND_SEQ_EVENT_CONTROLLER:
                sprintf(eventMessage, "%s,0,%d,%d,%d", deviceId, ev->data.control.channel, ev->data.control.param, ev->data.control.value);
                UnitySendMessage(GAME_OBJECT_NAME, "OnMidiControlChange", eventMessage);
                break;
            case SND_SEQ_EVENT_PGMCHANGE:
                sprintf(eventMessage, "%s,0,%d,%d", deviceId, ev->data.control.channel, ev->data.control.value);
                UnitySendMessage(GAME_OBJECT_NAME, "OnMidiProgramChange", eventMessage);
                break;
            case SND_SEQ_EVENT_CHANPRESS:
                sprintf(eventMessage, "%s,0,%d,%d", deviceId, ev->data.control.channel, ev->data.control.value);
                UnitySendMessage(GAME_OBJECT_NAME, "OnMidiChannelAftertouch", eventMessage);
                break;
            case SND_SEQ_EVENT_KEYPRESS:
                sprintf(eventMessage, "%s,0,%d,%d,%d", deviceId, ev->data.note.channel, ev->data.note.note, ev->data.note.velocity);
                UnitySendMessage(GAME_OBJECT_NAME, "OnMidiPolyphonicAftertouch", eventMessage);
                break;
            case SND_SEQ_EVENT_PITCHBEND:
                sprintf(eventMessage, "%s,0,%d,%d", deviceId, ev->data.control.channel, ev->data.control.value + 8192);
                UnitySendMessage(GAME_OBJECT_NAME, "OnMidiPitchWheel", eventMessage);
                break;
            case SND_SEQ_EVENT_SYSEX:
                {
                    std::vector<unsigned char> systemExclusiveStream;
                    for (int i = 0; i < ev->data.ext.len; ++i) {
                        systemExclusiveStream.push_back(((unsigned char *)(ev->data.ext.ptr))[i]);
                    }
                    std::ostringstream oss;
                    oss << deviceId;
                    oss << ",0,";
                    std::copy(systemExclusiveStream.begin(), systemExclusiveStream.end(), std::ostream_iterator<int>(oss, ","));
                    systemExclusiveStream.clear();

                    UnitySendMessage(GAME_OBJECT_NAME, "OnMidiSystemExclusive", oss.str().c_str());
                }
                break;
            case SND_SEQ_EVENT_SONGPOS:
                sprintf(eventMessage, "%s,0,%d", deviceId, ev->data.control.value);
                UnitySendMessage(GAME_OBJECT_NAME, "OnMidiSongPositionPointer", eventMessage);
                break;
            case SND_SEQ_EVENT_SONGSEL:
                sprintf(eventMessage, "%s,0,%d", deviceId, ev->data.control.value);
                UnitySendMessage(GAME_OBJECT_NAME, "OnMidiSongSelect", eventMessage);
                break;
            case SND_SEQ_EVENT_QFRAME:
                sprintf(eventMessage, "%s,0,%d", deviceId, ev->data.control.value);
                UnitySendMessage(GAME_OBJECT_NAME, "OnMidiTimeCodeQuarterFrame", eventMessage);
                break;
            case SND_SEQ_EVENT_TUNE_REQUEST:
                UnitySendMessage(GAME_OBJECT_NAME, "OnMidiTuneRequest", deviceId);
                break;
            case SND_SEQ_EVENT_CLOCK:
                UnitySendMessage(GAME_OBJECT_NAME, "OnMidiTimingClock", deviceId);
                break;
            case SND_SEQ_EVENT_START:
                UnitySendMessage(GAME_OBJECT_NAME, "OnMidiStart", deviceId);
                break;
            case SND_SEQ_EVENT_CONTINUE:
                UnitySendMessage(GAME_OBJECT_NAME, "OnMidiContinue", deviceId);
                break;
            case SND_SEQ_EVENT_STOP:
                UnitySendMessage(GAME_OBJECT_NAME, "OnMidiStop", deviceId);
                break;
            case SND_SEQ_EVENT_SENSING:
                UnitySendMessage(GAME_OBJECT_NAME, "OnMidiActiveSensing", deviceId);
                break;
            case SND_SEQ_EVENT_RESET:
                UnitySendMessage(GAME_OBJECT_NAME, "OnMidiReset", deviceId);
                break;
        }
    }
}

void midiEventWatcher(std::string deviceIdStr, snd_rawmidi_t* midiInput) {
    using namespace std::chrono_literals;
    ssize_t read;
    unsigned char buffer[1024];

    // states
    const int MIDI_STATE_WAIT = 0;
    const int MIDI_STATE_SIGNAL_2BYTES_2 = 21;
    const int MIDI_STATE_SIGNAL_3BYTES_2 = 31;
    const int MIDI_STATE_SIGNAL_3BYTES_3 = 32;
    const int MIDI_STATE_SIGNAL_SYSEX = 41;

    unsigned char midiEventKind;
    unsigned char midiEventNote;
    unsigned char midiEventVelocity;
    int midiState = MIDI_STATE_WAIT;
    std::vector<unsigned char> systemExclusiveStream;
    char eventMessage[128];
    const char* deviceId = deviceIdStr.c_str();

    while (!isStopped) {
        read = snd_rawmidi_read(midiInput, buffer, sizeof(buffer));
        if (read < 0) {
            // failed, stop this device
            snd_rawmidi_close(midiInput);
            break;
        }

        if (read > 0) {
            // parse MIDI
            for (int i = 0; i < read; i++) {
                unsigned char midiEvent = buffer[i];

                if (midiState == MIDI_STATE_WAIT) {
                    switch (midiEvent & 0xf0) {
                        case 0xf0: {
                            switch (midiEvent) {
                                case 0xf0:
                                    systemExclusiveStream.clear();
                                    systemExclusiveStream.push_back(midiEvent);
                                    midiState = MIDI_STATE_SIGNAL_SYSEX;
                                    break;

                                case 0xf1:
                                case 0xf3:
                                    // 0xf1 MIDI Time Code Quarter Frame. : 2bytes
                                    // 0xf3 Song Select. : 2bytes
                                    midiEventKind = midiEvent;
                                    midiState = MIDI_STATE_SIGNAL_2BYTES_2;
                                    break;

                                case 0xf2:
                                    // 0xf2 Song Position Pointer. : 3bytes
                                    midiEventKind = midiEvent;
                                    midiState = MIDI_STATE_SIGNAL_3BYTES_2;
                                    break;

                                case 0xf6:
                                    // 0xf6 Tune Request : 1byte
                                    UnitySendMessage(GAME_OBJECT_NAME, "OnMidiTuneRequest", deviceId);
                                    midiState = MIDI_STATE_WAIT;
                                    break;
                                case 0xf8:
                                    // 0xf8 Timing Clock : 1byte
                                    UnitySendMessage(GAME_OBJECT_NAME, "OnMidiTimingClock", deviceId);
                                    midiState = MIDI_STATE_WAIT;
                                    break;
                                case 0xfa:
                                    // 0xfa Start : 1byte
                                    UnitySendMessage(GAME_OBJECT_NAME, "OnMidiStart", deviceId);
                                    midiState = MIDI_STATE_WAIT;
                                    break;
                                case 0xfb:
                                    // 0xfb Continue : 1byte
                                    UnitySendMessage(GAME_OBJECT_NAME, "OnMidiContinue", deviceId);
                                    midiState = MIDI_STATE_WAIT;
                                    break;
                                case 0xfc:
                                    // 0xfc Stop : 1byte
                                    UnitySendMessage(GAME_OBJECT_NAME, "OnMidiStop", deviceId);
                                    midiState = MIDI_STATE_WAIT;
                                    break;
                                case 0xfe:
                                    // 0xfe Active Sensing : 1byte
                                    UnitySendMessage(GAME_OBJECT_NAME, "OnMidiActiveSensing", deviceId);
                                    midiState = MIDI_STATE_WAIT;
                                    break;
                                case 0xff:
                                    // 0xff Reset : 1byte
                                    UnitySendMessage(GAME_OBJECT_NAME, "OnMidiReset", deviceId);
                                    midiState = MIDI_STATE_WAIT;
                                    break;

                                default:
                                    break;
                            }
                        }
                        break;
                        case 0x80:
                        case 0x90:
                        case 0xa0:
                        case 0xb0:
                        case 0xe0:
                            // 3bytes pattern
                            midiEventKind = midiEvent;
                            midiState = MIDI_STATE_SIGNAL_3BYTES_2;
                            break;
                        case 0xc0: // program change
                        case 0xd0: // channel after-touch
                            // 2bytes pattern
                            midiEventKind = midiEvent;
                            midiState = MIDI_STATE_SIGNAL_2BYTES_2;
                            break;
                        default:
                            // 0x00 - 0x70: running status
                            if ((midiEventKind & 0xf0) != 0xf0) {
                                    // previous event kind is multi-bytes pattern
                                    midiEventNote = midiEvent;
                                    midiState = MIDI_STATE_SIGNAL_3BYTES_3;
                            }
                            break;
                    }
                } else if (midiState == MIDI_STATE_SIGNAL_2BYTES_2) {
                    switch (midiEventKind & 0xf0) {
                        // 2bytes pattern
                        case 0xc0: // program change
                            midiEventNote = midiEvent;
                            sprintf(eventMessage, "%s,0,%d,%d", deviceId, midiEventKind & 0xf, midiEventNote);
                            UnitySendMessage(GAME_OBJECT_NAME, "OnMidiProgramChange", eventMessage);
                            midiState = MIDI_STATE_WAIT;
                            break;
                        case 0xd0: // channel after-touch
                            midiEventNote = midiEvent;
                            sprintf(eventMessage, "%s,0,%d,%d", deviceId, midiEventKind & 0xf, midiEventNote);
                            UnitySendMessage(GAME_OBJECT_NAME, "OnMidiChannelAftertouch", eventMessage);
                            midiState = MIDI_STATE_WAIT;
                            break;
                        case 0xf0: {
                            switch (midiEventKind) {
                                case 0xf1:
                                    // 0xf1 MIDI Time Code Quarter Frame. : 2bytes
                                    midiEventNote = midiEvent;
                                    sprintf(eventMessage, "%s,0,%d", deviceId, midiEventNote);
                                    UnitySendMessage(GAME_OBJECT_NAME, "OnMidiTimeCodeQuarterFrame", eventMessage);
                                    midiState = MIDI_STATE_WAIT;
                                    break;
                                case 0xf3:
                                    // 0xf3 Song Select. : 2bytes
                                    midiEventNote = midiEvent;
                                    sprintf(eventMessage, "%s,0,%d", deviceId, midiEventNote);
                                    UnitySendMessage(GAME_OBJECT_NAME, "OnMidiSongSelect", eventMessage);
                                    midiState = MIDI_STATE_WAIT;
                                    break;
                                default:
                                    // illegal state
                                    midiState = MIDI_STATE_WAIT;
                                    break;
                            }
                        }
                            break;
                        default:
                            // illegal state
                            midiState = MIDI_STATE_WAIT;
                            break;
                    }
                } else if (midiState == MIDI_STATE_SIGNAL_3BYTES_2) {
                    switch (midiEventKind & 0xf0) {
                        case 0x80:
                        case 0x90:
                        case 0xa0:
                        case 0xb0:
                        case 0xe0:
                        case 0xf0:
                            // 3bytes pattern
                            midiEventNote = midiEvent;
                            midiState = MIDI_STATE_SIGNAL_3BYTES_3;
                            break;
                        default:
                            // illegal state
                            midiState = MIDI_STATE_WAIT;
                            break;
                    }
                } else if (midiState == MIDI_STATE_SIGNAL_3BYTES_3) {
                    switch (midiEventKind & 0xf0) {
                        // 3bytes pattern
                        case 0x80: // note off
                            midiEventVelocity = midiEvent;
                            sprintf(eventMessage, "%s,0,%d,%d,%d", deviceId, midiEventKind & 0xf, midiEventNote, midiEventVelocity);
                            UnitySendMessage(GAME_OBJECT_NAME, "OnMidiNoteOff", eventMessage);
                            midiState = MIDI_STATE_WAIT;
                            break;
                        case 0x90: // note on
                            midiEventVelocity = midiEvent;
                            sprintf(eventMessage, "%s,0,%d,%d,%d", deviceId, midiEventKind & 0xf, midiEventNote, midiEventVelocity);
                            UnitySendMessage(GAME_OBJECT_NAME, "OnMidiNoteOn", eventMessage);
                            midiState = MIDI_STATE_WAIT;
                            break;
                        case 0xa0: // control polyphonic key pressure
                            midiEventVelocity = midiEvent;
                            sprintf(eventMessage, "%s,0,%d,%d,%d", deviceId, midiEventKind & 0xf, midiEventNote, midiEventVelocity);
                            UnitySendMessage(GAME_OBJECT_NAME, "OnMidiPolyphonicAftertouch", eventMessage);
                            midiState = MIDI_STATE_WAIT;
                            break;
                        case 0xb0: // control change
                            midiEventVelocity = midiEvent;
                            sprintf(eventMessage, "%s,0,%d,%d,%d", deviceId, midiEventKind & 0xf, midiEventNote, midiEventVelocity);
                            UnitySendMessage(GAME_OBJECT_NAME, "OnMidiControlChange", eventMessage);
                            midiState = MIDI_STATE_WAIT;
                            break;
                        case 0xe0: // pitch bend
                            midiEventVelocity = midiEvent;
                            sprintf(eventMessage, "%s,0,%d,%d", deviceId, midiEventKind & 0xf, (midiEventNote & 0x7f) | ((midiEventVelocity & 0x7f) << 7));
                            UnitySendMessage(GAME_OBJECT_NAME, "OnMidiPitchWheel", eventMessage);
                            midiState = MIDI_STATE_WAIT;
                            break;
                        case 0xf0: // Song Position Pointer.
                            midiEventVelocity = midiEvent;
                            sprintf(eventMessage, "%s,0,%d", deviceId, (midiEventNote & 0x7f) | ((midiEventVelocity & 0x7f) << 7));
                            UnitySendMessage(GAME_OBJECT_NAME, "OnMidiSongPositionPointer", eventMessage);
                            midiState = MIDI_STATE_WAIT;
                            break;
                        default:
                            // illegal state
                            midiState = MIDI_STATE_WAIT;
                            break;
                    }
                } else if (midiState == MIDI_STATE_SIGNAL_SYSEX) {
                    if (midiEvent == 0xf7) {
                        // the end of message
                        if (!systemExclusiveStream.empty()) {
                            std::ostringstream oss;
                            oss << deviceId;
                            oss << ",0,";
                            std::copy(systemExclusiveStream.begin(), systemExclusiveStream.end(), std::ostream_iterator<int>(oss, ","));
                            oss << (int)midiEvent;

                            UnitySendMessage(GAME_OBJECT_NAME, "OnMidiSystemExclusive", oss.str().c_str());
                        }
                        systemExclusiveStream.clear();

                        midiState = MIDI_STATE_WAIT;
                    } else {
                        systemExclusiveStream.push_back(midiEvent);
                    }
                }
            }
        }

        std::this_thread::sleep_for(10ms);
    }
}

#define ENABLE_RAWMIDI
#define LIST_INPUT    1
#define LIST_OUTPUT    2
#define perm_ok(cap,bits) (((cap) & (bits)) == (bits))
static int check_permission(snd_seq_port_info_t *pinfo, int perm)
{
    int cap = snd_seq_port_info_get_capability(pinfo);

    if (cap & SND_SEQ_PORT_CAP_NO_EXPORT) {
        return 0;
    }

    if (perm & LIST_INPUT) {
        if (perm_ok(cap, SND_SEQ_PORT_CAP_READ|SND_SEQ_PORT_CAP_SUBS_READ)) {
            return 1;
        }
    }
    if (perm & LIST_OUTPUT) {
        if (perm_ok(cap, SND_SEQ_PORT_CAP_WRITE|SND_SEQ_PORT_CAP_SUBS_WRITE)) {
            return 1;
        }
    }
    return 0;
}

void midiConnectionWatcher() {
    using namespace std::chrono_literals;

    char deviceId[32];

#ifdef ENABLE_RAWMIDI
    // rawmidi
    int status;
    int card;

    snd_ctl_t *ctl;
    char name[32];
    int device;
    char sub_name[32];

    char* deviceName = NULL;

    snd_rawmidi_info_t *info;
    int subs;
    int sub;

    snd_rawmidi_info_alloca(&info);
#endif

    // virtual midi
    int client;
    int port;

    snd_seq_client_info_t *cinfo;
    snd_seq_port_info_t *pinfo;

    snd_seq_client_info_alloca(&cinfo);
    snd_seq_port_info_alloca(&pinfo);

    // current connections to detect detached
    std::set<std::string> currentConnections;
    std::set<std::string> connectionsToRemove;

    while (!isStopped) {
        currentConnections.clear();
        snd_seq_client_info_set_client(cinfo, -1);
        while (snd_seq_query_next_client(seq_handle, cinfo) >= 0) {
            // loop with client
            if (snd_seq_client_info_get_type(cinfo) == SND_SEQ_KERNEL_CLIENT) {
                // system client: ignore
                continue;
            }

            // reset query info
            snd_seq_port_info_set_client(pinfo, snd_seq_client_info_get_client(cinfo));
            snd_seq_port_info_set_port(pinfo, -1);

            while (snd_seq_query_next_port(seq_handle, pinfo) >= 0) {
                // loop with port
                snd_seq_addr_t addr;
                addr.client = snd_seq_client_info_get_client(cinfo);
                addr.port = snd_seq_port_info_get_port(pinfo);

                if (addr.client == selfClientId && addr.port == selfPortNumber) {
                    // self client: ignore
                    continue;
                }

                if (check_permission(pinfo, LIST_INPUT)) {
                    // found a input port
                    sprintf(deviceId, "seq:%d-%d", addr.client, addr.port);
                    currentConnections.insert(deviceId);

                    if (virtualMidiInputMap.find(deviceId) == virtualMidiInputMap.end()) {
                        const char* deviceName = snd_seq_client_info_get_name(cinfo);
                        if (deviceNames.find(deviceId) == deviceNames.end()) {
                            deviceNames.insert(std::make_pair(deviceId, deviceName));
                        }
                        virtualMidiInputMap.insert(std::make_pair(deviceId, addr));

                        UnitySendMessage(GAME_OBJECT_NAME, "OnMidiInputDeviceAttached", deviceId);
                    }
                }

                if (check_permission(pinfo, LIST_OUTPUT)) {
                    // found a output port
                    sprintf(deviceId, "seq:%d-%d", addr.client, addr.port);
                    currentConnections.insert(deviceId);

                    if (virtualMidiOutputMap.find(deviceId) == virtualMidiOutputMap.end()) {
                        const char* deviceName = snd_seq_client_info_get_name(cinfo);
                        if (deviceNames.find(deviceId) == deviceNames.end()) {
                            deviceNames.insert(std::make_pair(deviceId, deviceName));
                        }
                        virtualMidiOutputMap.insert(std::make_pair(deviceId, addr));

                        UnitySendMessage(GAME_OBJECT_NAME, "OnMidiOutputDeviceAttached", deviceId);
                    }
                }
            }
        }

        connectionsToRemove.clear();
        for (std::map<std::string, snd_seq_addr_t>::iterator it = virtualMidiInputMap.begin(); it != virtualMidiInputMap.end(); ++it) {
            if (currentConnections.find(it->first) == currentConnections.end()) {
                UnitySendMessage(GAME_OBJECT_NAME, "OnMidiInputDeviceDetached", it->first.c_str());
                connectionsToRemove.insert(it->first);
            }
        }
        for (std::set<std::string>::iterator it = connectionsToRemove.begin(); it != connectionsToRemove.end(); ++it) {
            virtualMidiInputMap.erase(*it);
        }
        connectionsToRemove.clear();
        for (std::map<std::string, snd_seq_addr_t>::iterator it = virtualMidiOutputMap.begin(); it != virtualMidiOutputMap.end(); ++it) {
            if (currentConnections.find(it->first) == currentConnections.end()) {
                UnitySendMessage(GAME_OBJECT_NAME, "OnMidiOutputDeviceDetached", it->first.c_str());
                connectionsToRemove.insert(it->first);
            }
        }
        for (std::set<std::string>::iterator it = connectionsToRemove.begin(); it != connectionsToRemove.end(); ++it) {
            virtualMidiOutputMap.erase(*it);
        }

#ifdef ENABLE_RAWMIDI
        currentConnections.clear();
        card = -1;
        if ((status = snd_card_next(&card)) >= 0 && (card >= 0)) {
            while (card >= 0) {
                sprintf(name, "hw:%d", card);
                if ((status = snd_ctl_open(&ctl, name, 0)) < 0) {
                    continue;
                }
                snd_card_get_name(card, &deviceName);
                device = -1;
                do {
                    status = snd_ctl_rawmidi_next_device(ctl, &device);
                    if (status < 0) {
                        break;
                    }
                    if (device >= 0) {
                        snd_rawmidi_info_set_device(info, device);

                        // sub devices: input
                        snd_rawmidi_info_set_stream(info, SND_RAWMIDI_STREAM_INPUT);
                        snd_ctl_rawmidi_info(ctl, info);
                        subs = snd_rawmidi_info_get_subdevices_count(info);
                        for (sub = 0; sub < subs; sub++) {
                            sprintf(sub_name, "hw:%d,%d,%d", card, device, sub);
                            sprintf(deviceId, "hw:%d-%d-%d", card, device, sub);
                            currentConnections.insert(deviceId);

                            if (midiInputMap.find(deviceId) == midiInputMap.end()) {
                                snd_rawmidi_t* midiInput = NULL;
                                snd_rawmidi_open(&midiInput, NULL, sub_name, SND_RAWMIDI_SYNC);
                                if (midiInput) {
                                    if (deviceNames.find(deviceId) == deviceNames.end()) {
                                        deviceNames.insert(std::make_pair(deviceId, deviceName));
                                    }
                                    midiInputMap.insert(std::make_pair(deviceId, midiInput));

                                    // input watcher thread
                                    std::string deviceIdStr = deviceId;
                                    std::thread midiInputThread(midiEventWatcher, deviceIdStr, midiInput);
                                    midiInputThread.detach();

                                    UnitySendMessage(GAME_OBJECT_NAME, "OnMidiInputDeviceAttached", deviceId);
                                }
                            }
                        }

                        // sub devices: output
                        snd_rawmidi_info_set_stream(info, SND_RAWMIDI_STREAM_OUTPUT);
                        snd_ctl_rawmidi_info(ctl, info);
                        subs = snd_rawmidi_info_get_subdevices_count(info);
                        for (sub = 0; sub < subs; sub++) {
                            sprintf(sub_name, "hw:%d,%d,%d", card, device, sub);
                            sprintf(deviceId, "hw:%d-%d-%d", card, device, sub);
                            currentConnections.insert(deviceId);

                            if (midiOutputMap.find(deviceId) == midiOutputMap.end()) {
                                snd_rawmidi_t* midiOutput = NULL;
                                snd_rawmidi_open(NULL, &midiOutput, sub_name, SND_RAWMIDI_SYNC);
                                if (midiOutput) {
                                    if (deviceNames.find(deviceId) == deviceNames.end()) {
                                        deviceNames.insert(std::make_pair(deviceId, deviceName));
                                    }
                                    midiOutputMap.insert(std::make_pair(deviceId, midiOutput));

                                    UnitySendMessage(GAME_OBJECT_NAME, "OnMidiOutputDeviceAttached", deviceId);
                                }
                            }
                        }
                    }
                } while (device >= 0);
                snd_ctl_close(ctl);

                if ((status = snd_card_next(&card)) < 0) {
                    break;
                }
            }
        }

        connectionsToRemove.clear();
        for (std::map<std::string, snd_rawmidi_t*>::iterator it = midiInputMap.begin(); it != midiInputMap.end(); ++it) {
            if (currentConnections.find(it->first) == currentConnections.end()) {
                UnitySendMessage(GAME_OBJECT_NAME, "OnMidiInputDeviceDetached", it->first.c_str());
                connectionsToRemove.insert(it->first);
            }
        }
        for (std::set<std::string>::iterator it = connectionsToRemove.begin(); it != connectionsToRemove.end(); ++it) {
            midiInputMap.erase(*it);
        }
        connectionsToRemove.clear();
        for (std::map<std::string, snd_rawmidi_t*>::iterator it = midiOutputMap.begin(); it != midiOutputMap.end(); ++it) {
            if (currentConnections.find(it->first) == currentConnections.end()) {
                UnitySendMessage(GAME_OBJECT_NAME, "OnMidiOutputDeviceDetached", it->first.c_str());
                connectionsToRemove.insert(it->first);
            }
        }
        for (std::set<std::string>::iterator it = connectionsToRemove.begin(); it != connectionsToRemove.end(); ++it) {
            midiOutputMap.erase(*it);
        }
#endif

        std::this_thread::sleep_for(100ms);
        }
}

void SetSendMessageCallback(OnSendMessageDelegate callback) {
   onSendMessage = callback;
}

void InitializeMidiLinux() {
    if (seq_handle == nullptr) {
        snd_seq_open(&seq_handle, "default", SND_SEQ_OPEN_DUPLEX, 0);
        snd_seq_set_client_name(seq_handle, "Midi Handler");
        selfClientId = snd_seq_client_id(seq_handle);
        selfPortNumber = snd_seq_create_simple_port(seq_handle, "inout",
            SND_SEQ_PORT_CAP_READ|SND_SEQ_PORT_CAP_SUBS_READ|SND_SEQ_PORT_CAP_WRITE|SND_SEQ_PORT_CAP_SUBS_WRITE,
            SND_SEQ_PORT_TYPE_APPLICATION);
    }

    isStopped = false;
    std::thread midiConnectionThread(midiConnectionWatcher);
    midiConnectionThread.detach();

    // input watcher thread
    std::thread midiInputThread(virtualMidiEventWatcher);
    midiInputThread.detach();
}

void TerminateMidiLinux() {
    isStopped = true;
}

const char* GetDeviceNameLinux(const char* deviceId) {
    if (deviceNames.find(deviceId) != deviceNames.end()) {
        return strdup(deviceNames[deviceId].c_str());
    }

    return NULL;
}

void SendMidiNoteOff(const char* deviceId, char channel, char note, char velocity) {
    decltype(midiOutputMap)::iterator it = midiOutputMap.find(deviceId);
    if (it != midiOutputMap.end()) {
        char midi[3] = {(char)(0x80 | channel), note, velocity};
        snd_rawmidi_write(it->second, midi, 3);
    }

    decltype(virtualMidiOutputMap)::iterator it2 = virtualMidiOutputMap.find(deviceId);
    if (it2 != virtualMidiOutputMap.end() && seq_handle != nullptr) {
        snd_seq_event_t ev;
        snd_seq_ev_clear(&ev);
        snd_seq_ev_set_direct(&ev);

        snd_seq_ev_set_dest(&ev, it2->second.client, it2->second.port);

        snd_seq_ev_set_noteoff(&ev, channel, note, velocity);
        snd_seq_event_output(seq_handle, &ev);
        snd_seq_drain_output(seq_handle);
    }
}

void SendMidiNoteOn(const char* deviceId, char channel, char note, char velocity) {
    decltype(midiOutputMap)::iterator it = midiOutputMap.find(deviceId);
    if (it != midiOutputMap.end()) {
        char midi[3] = {(char)(0x90 | channel), note, velocity};
        snd_rawmidi_write(it->second, midi, 3);
    }

    decltype(virtualMidiOutputMap)::iterator it2 = virtualMidiOutputMap.find(deviceId);
    if (it2 != virtualMidiOutputMap.end() && seq_handle != nullptr) {
        snd_seq_event_t ev;
        snd_seq_ev_clear(&ev);
        snd_seq_ev_set_direct(&ev);

        snd_seq_ev_set_dest(&ev, it2->second.client, it2->second.port);

        snd_seq_ev_set_noteon(&ev, channel, note, velocity);
        snd_seq_event_output(seq_handle, &ev);
        snd_seq_drain_output(seq_handle);
    }
}

void SendMidiPolyphonicAftertouch(const char* deviceId, char channel, char note, char pressure) {
    decltype(midiOutputMap)::iterator it = midiOutputMap.find(deviceId);
    if (it != midiOutputMap.end()) {
        char midi[3] = {(char)(0xa0 | channel), note, pressure};
        snd_rawmidi_write(it->second, midi, 3);
    }

    decltype(virtualMidiOutputMap)::iterator it2 = virtualMidiOutputMap.find(deviceId);
    if (it2 != virtualMidiOutputMap.end() && seq_handle != nullptr) {
        snd_seq_event_t ev;
        snd_seq_ev_clear(&ev);
        snd_seq_ev_set_direct(&ev);

        snd_seq_ev_set_dest(&ev, it2->second.client, it2->second.port);

        snd_seq_ev_set_keypress(&ev, channel, note, pressure);
        snd_seq_event_output(seq_handle, &ev);
        snd_seq_drain_output(seq_handle);
    }
}

void SendMidiControlChange(const char* deviceId, char channel, char func, char value) {
    decltype(midiOutputMap)::iterator it = midiOutputMap.find(deviceId);
    if (it != midiOutputMap.end()) {
        char midi[3] = {(char)(0xb0 | channel), func, value};
        snd_rawmidi_write(it->second, midi, 3);
    }

    decltype(virtualMidiOutputMap)::iterator it2 = virtualMidiOutputMap.find(deviceId);
    if (it2 != virtualMidiOutputMap.end() && seq_handle != nullptr) {
        snd_seq_event_t ev;
        snd_seq_ev_clear(&ev);
        snd_seq_ev_set_direct(&ev);

        snd_seq_ev_set_dest(&ev, it2->second.client, it2->second.port);

        snd_seq_ev_set_controller(&ev, channel, func, value);
        snd_seq_event_output(seq_handle, &ev);
        snd_seq_drain_output(seq_handle);
    }
}

void SendMidiProgramChange(const char* deviceId, char channel, char program) {
    decltype(midiOutputMap)::iterator it = midiOutputMap.find(deviceId);
    if (it != midiOutputMap.end()) {
        char midi[2] = {(char)(0xc0 | channel), program};
        snd_rawmidi_write(it->second, midi, 2);
    }

    decltype(virtualMidiOutputMap)::iterator it2 = virtualMidiOutputMap.find(deviceId);
    if (it2 != virtualMidiOutputMap.end() && seq_handle != nullptr) {
        snd_seq_event_t ev;
        snd_seq_ev_clear(&ev);
        snd_seq_ev_set_direct(&ev);

        snd_seq_ev_set_dest(&ev, it2->second.client, it2->second.port);

        snd_seq_ev_set_pgmchange(&ev, channel, program);
        snd_seq_event_output(seq_handle, &ev);
        snd_seq_drain_output(seq_handle);
    }
}

void SendMidiChannelAftertouch(const char* deviceId, char channel, char pressure) {
    decltype(midiOutputMap)::iterator it = midiOutputMap.find(deviceId);
    if (it != midiOutputMap.end()) {
        char midi[2] = {(char)(0xd0 | channel), pressure};
        snd_rawmidi_write(it->second, midi, 2);
    }

    decltype(virtualMidiOutputMap)::iterator it2 = virtualMidiOutputMap.find(deviceId);
    if (it2 != virtualMidiOutputMap.end() && seq_handle != nullptr) {
        snd_seq_event_t ev;
        snd_seq_ev_clear(&ev);
        snd_seq_ev_set_direct(&ev);

        snd_seq_ev_set_dest(&ev, it2->second.client, it2->second.port);

        snd_seq_ev_set_chanpress(&ev, channel, pressure);
        snd_seq_event_output(seq_handle, &ev);
        snd_seq_drain_output(seq_handle);
    }
}

void SendMidiPitchWheel(const char* deviceId, char channel, short amount) {
    decltype(midiOutputMap)::iterator it = midiOutputMap.find(deviceId);
    if (it != midiOutputMap.end()) {
        char midi[3] = {(char)(0xe0 | channel), (char)(amount & 0x7f), (char)((amount >> 7) & 0x7f)};
        snd_rawmidi_write(it->second, midi, 3);
    }

    decltype(virtualMidiOutputMap)::iterator it2 = virtualMidiOutputMap.find(deviceId);
    if (it2 != virtualMidiOutputMap.end() && seq_handle != nullptr) {
        snd_seq_event_t ev;
        snd_seq_ev_clear(&ev);
        snd_seq_ev_set_direct(&ev);

        snd_seq_ev_set_dest(&ev, it2->second.client, it2->second.port);

        snd_seq_ev_set_pitchbend(&ev, channel, amount - 8192);
        snd_seq_event_output(seq_handle, &ev);
        snd_seq_drain_output(seq_handle);
    }
}

void SendMidiSystemExclusive(const char* deviceId, unsigned char* data, int length) {
    decltype(midiOutputMap)::iterator it = midiOutputMap.find(deviceId);
    if (it != midiOutputMap.end()) {
        snd_rawmidi_write(it->second, data, length);
    }

    decltype(virtualMidiOutputMap)::iterator it2 = virtualMidiOutputMap.find(deviceId);
    if (it2 != virtualMidiOutputMap.end() && seq_handle != nullptr) {
        snd_seq_event_t ev;
        snd_seq_ev_clear(&ev);
        snd_seq_ev_set_direct(&ev);

        snd_seq_ev_set_dest(&ev, it2->second.client, it2->second.port);

        snd_seq_ev_set_sysex(&ev, length, data);
        snd_seq_event_output(seq_handle, &ev);
        snd_seq_drain_output(seq_handle);
    }
}

void SendMidiTimeCodeQuarterFrame(const char* deviceId, char value) {
    decltype(midiOutputMap)::iterator it = midiOutputMap.find(deviceId);
    if (it != midiOutputMap.end()) {
        char midi[2] = {(char)0xf1, value};
        snd_rawmidi_write(it->second, midi, 2);
    }
}

void SendMidiSongPositionPointer(const char* deviceId, short position) {
    decltype(midiOutputMap)::iterator it = midiOutputMap.find(deviceId);
    if (it != midiOutputMap.end()) {
        char midi[3] = {(char)0xf2, (char)(position & 0x7f), (char)((position >> 7) & 0x7f)};
        snd_rawmidi_write(it->second, midi, 3);
    }
}

void SendMidiSongSelect(const char* deviceId, char song) {
    decltype(midiOutputMap)::iterator it = midiOutputMap.find(deviceId);
    if (it != midiOutputMap.end()) {
        char midi[2] = {(char)0xf3, song};
        snd_rawmidi_write(it->second, midi, 2);
    }
}

void SendMidiTuneRequest(const char* deviceId) {
    decltype(midiOutputMap)::iterator it = midiOutputMap.find(deviceId);
    if (it != midiOutputMap.end()) {
        char midi[1] = {(char)0xf6};
        snd_rawmidi_write(it->second, midi, 1);
    }
}

void SendMidiTimingClock(const char* deviceId) {
    decltype(midiOutputMap)::iterator it = midiOutputMap.find(deviceId);
    if (it != midiOutputMap.end()) {
        char midi[1] = {(char)0xf8};
        snd_rawmidi_write(it->second, midi, 1);
    }
}

void SendMidiStart(const char* deviceId) {
    decltype(midiOutputMap)::iterator it = midiOutputMap.find(deviceId);
    if (it != midiOutputMap.end()) {
        char midi[1] = {(char)0xfa};
        snd_rawmidi_write(it->second, midi, 1);
    }
}

void SendMidiContinue(const char* deviceId) {
    decltype(midiOutputMap)::iterator it = midiOutputMap.find(deviceId);
    if (it != midiOutputMap.end()) {
        char midi[1] = {(char)0xfb};
        snd_rawmidi_write(it->second, midi, 1);
    }
}

void SendMidiStop(const char* deviceId) {
    decltype(midiOutputMap)::iterator it = midiOutputMap.find(deviceId);
    if (it != midiOutputMap.end()) {
        char midi[1] = {(char)0xfc};
        snd_rawmidi_write(it->second, midi, 1);
    }
}

void SendMidiActiveSensing(const char* deviceId) {
    decltype(midiOutputMap)::iterator it = midiOutputMap.find(deviceId);
    if (it != midiOutputMap.end()) {
        char midi[1] = {(char)0xfe};
        snd_rawmidi_write(it->second, midi, 1);
    }
}

void SendMidiReset(const char* deviceId) {
    decltype(midiOutputMap)::iterator it = midiOutputMap.find(deviceId);
    if (it != midiOutputMap.end()) {
        char midi[1] = {(char)0xff};
        snd_rawmidi_write(it->second, midi, 1);
    }
}

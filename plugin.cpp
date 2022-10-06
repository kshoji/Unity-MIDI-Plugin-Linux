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
std::map<std::string, std::string> deviceNames;

const char *GAME_OBJECT_NAME = "MidiManager";

volatile bool isStopped;

OnSendMessageDelegate onSendMessage;

void UnitySendMessage(const char* obj, const char* method, const char* msg) {
    if (onSendMessage) {
        onSendMessage(method, msg);
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
            midiInputMap.erase(deviceId);
            UnitySendMessage(GAME_OBJECT_NAME, "OnMidiInputDeviceDetached", deviceId);

            midiOutputMap.erase(deviceId);
            UnitySendMessage(GAME_OBJECT_NAME, "OnMidiOutputDeviceDetached", deviceId);
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

void midiConnectionWatcher() {
    using namespace std::chrono_literals;
    int status;
    int card;

    snd_ctl_t *ctl;
    char name[32];
    int device;
    char sub_name[32];
    char deviceId[32];

    char* deviceName = NULL;

    snd_rawmidi_info_t *info;
    int subs;
    int sub;

    snd_rawmidi_info_alloca(&info);

    while (!isStopped) {
        card = -1;
        if ((status = snd_card_next(&card)) < 0) {
            continue;
        }
        if (card < 0) {
            continue;
        }

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

        std::this_thread::sleep_for(100ms);
    }
}

void SetSendMessageCallback(OnSendMessageDelegate callback) {
   onSendMessage = callback;
}

void InitializeMidiLinux() {
    isStopped = false;
    std::thread midiConnectionThread(midiConnectionWatcher);
    midiConnectionThread.detach();
}

void TerminateMidiLinux() {
    isStopped = true;
}

void SendMidiNoteOff(const char* deviceId, char channel, char note, char velocity) {
    decltype(midiOutputMap)::iterator it = midiOutputMap.find(deviceId);
    if (it != midiOutputMap.end()) {
        char midi[3] = {(char)(0x80 | channel), note, velocity};
        int status = snd_rawmidi_write(it->second, midi, 3);
        if (status < 0) {
            midiOutputMap.erase(deviceId);
            UnitySendMessage(GAME_OBJECT_NAME, "OnMidiOutputDeviceDetached", deviceId);
        }
    }
}

void SendMidiNoteOn(const char* deviceId, char channel, char note, char velocity) {
    decltype(midiOutputMap)::iterator it = midiOutputMap.find(deviceId);
    if (it != midiOutputMap.end()) {
        char midi[3] = {(char)(0x90 | channel), note, velocity};
        int status = snd_rawmidi_write(it->second, midi, 3);
        if (status < 0) {
            midiOutputMap.erase(deviceId);
            UnitySendMessage(GAME_OBJECT_NAME, "OnMidiOutputDeviceDetached", deviceId);
        }
    }
}

void SendMidiPolyphonicAftertouch(const char* deviceId, char channel, char note, char pressure) {
    decltype(midiOutputMap)::iterator it = midiOutputMap.find(deviceId);
    if (it != midiOutputMap.end()) {
        char midi[3] = {(char)(0xa0 | channel), note, pressure};
        int status = snd_rawmidi_write(it->second, midi, 3);
        if (status < 0) {
            midiOutputMap.erase(deviceId);
            UnitySendMessage(GAME_OBJECT_NAME, "OnMidiOutputDeviceDetached", deviceId);
        }
    }
}

void SendMidiControlChange(const char* deviceId, char channel, char func, char value) {
    decltype(midiOutputMap)::iterator it = midiOutputMap.find(deviceId);
    if (it != midiOutputMap.end()) {
        char midi[3] = {(char)(0xb0 | channel), func, value};
        int status = snd_rawmidi_write(it->second, midi, 3);
        if (status < 0) {
            midiOutputMap.erase(deviceId);
            UnitySendMessage(GAME_OBJECT_NAME, "OnMidiOutputDeviceDetached", deviceId);
        }
    }
}

void SendMidiProgramChange(const char* deviceId, char channel, char program) {
    decltype(midiOutputMap)::iterator it = midiOutputMap.find(deviceId);
    if (it != midiOutputMap.end()) {
        char midi[2] = {(char)(0xc0 | channel), program};
        int status = snd_rawmidi_write(it->second, midi, 2);
        if (status < 0) {
            midiOutputMap.erase(deviceId);
            UnitySendMessage(GAME_OBJECT_NAME, "OnMidiOutputDeviceDetached", deviceId);
        }
    }
}

void SendMidiChannelAftertouch(const char* deviceId, char channel, char pressure) {
    decltype(midiOutputMap)::iterator it = midiOutputMap.find(deviceId);
    if (it != midiOutputMap.end()) {
        char midi[2] = {(char)(0xd0 | channel), pressure};
        int status = snd_rawmidi_write(it->second, midi, 2);
        if (status < 0) {
            midiOutputMap.erase(deviceId);
            UnitySendMessage(GAME_OBJECT_NAME, "OnMidiOutputDeviceDetached", deviceId);
        }
    }
}

void SendMidiPitchWheel(const char* deviceId, char channel, short amount) {
    decltype(midiOutputMap)::iterator it = midiOutputMap.find(deviceId);
    if (it != midiOutputMap.end()) {
        char midi[3] = {(char)(0xe0 | channel), (char)(amount & 0x7f), (char)((amount >> 7) & 0x7f)};
        int status = snd_rawmidi_write(it->second, midi, 3);
        if (status < 0) {
            midiOutputMap.erase(deviceId);
            UnitySendMessage(GAME_OBJECT_NAME, "OnMidiOutputDeviceDetached", deviceId);
        }
    }
}

void SendMidiSystemExclusive(const char* deviceId, unsigned char* data, int length) {
    decltype(midiOutputMap)::iterator it = midiOutputMap.find(deviceId);
    if (it != midiOutputMap.end()) {
        int status = snd_rawmidi_write(it->second, data, length);
        if (status < 0) {
            midiOutputMap.erase(deviceId);
            UnitySendMessage(GAME_OBJECT_NAME, "OnMidiOutputDeviceDetached", deviceId);
        }
    }
}

void SendMidiTimeCodeQuarterFrame(const char* deviceId, char value) {
    decltype(midiOutputMap)::iterator it = midiOutputMap.find(deviceId);
    if (it != midiOutputMap.end()) {
        char midi[2] = {(char)0xf1, value};
        int status = snd_rawmidi_write(it->second, midi, 2);
        if (status < 0) {
            midiOutputMap.erase(deviceId);
            UnitySendMessage(GAME_OBJECT_NAME, "OnMidiOutputDeviceDetached", deviceId);
        }
    }
}

void SendMidiSongPositionPointer(const char* deviceId, short position) {
    decltype(midiOutputMap)::iterator it = midiOutputMap.find(deviceId);
    if (it != midiOutputMap.end()) {
        char midi[3] = {(char)0xf2, (char)(position & 0x7f), (char)((position >> 7) & 0x7f)};
        int status = snd_rawmidi_write(it->second, midi, 3);
        if (status < 0) {
            midiOutputMap.erase(deviceId);
            UnitySendMessage(GAME_OBJECT_NAME, "OnMidiOutputDeviceDetached", deviceId);
        }
    }
}

void SendMidiSongSelect(const char* deviceId, char song) {
    decltype(midiOutputMap)::iterator it = midiOutputMap.find(deviceId);
    if (it != midiOutputMap.end()) {
        char midi[2] = {(char)0xf3, song};
        int status = snd_rawmidi_write(it->second, midi, 2);
        if (status < 0) {
            midiOutputMap.erase(deviceId);
            UnitySendMessage(GAME_OBJECT_NAME, "OnMidiOutputDeviceDetached", deviceId);
        }
    }
}

void SendMidiTuneRequest(const char* deviceId) {
    decltype(midiOutputMap)::iterator it = midiOutputMap.find(deviceId);
    if (it != midiOutputMap.end()) {
        char midi[1] = {(char)0xf6};
        int status = snd_rawmidi_write(it->second, midi, 1);
        if (status < 0) {
            midiOutputMap.erase(deviceId);
            UnitySendMessage(GAME_OBJECT_NAME, "OnMidiOutputDeviceDetached", deviceId);
        }
    }
}

void SendMidiTimingClock(const char* deviceId) {
    decltype(midiOutputMap)::iterator it = midiOutputMap.find(deviceId);
    if (it != midiOutputMap.end()) {
        char midi[1] = {(char)0xf8};
        int status = snd_rawmidi_write(it->second, midi, 1);
        if (status < 0) {
            midiOutputMap.erase(deviceId);
            UnitySendMessage(GAME_OBJECT_NAME, "OnMidiOutputDeviceDetached", deviceId);
        }
    }
}

void SendMidiStart(const char* deviceId) {
    decltype(midiOutputMap)::iterator it = midiOutputMap.find(deviceId);
    if (it != midiOutputMap.end()) {
        char midi[1] = {(char)0xfa};
        int status = snd_rawmidi_write(it->second, midi, 1);
        if (status < 0) {
            midiOutputMap.erase(deviceId);
            UnitySendMessage(GAME_OBJECT_NAME, "OnMidiOutputDeviceDetached", deviceId);
        }
    }
}

void SendMidiContinue(const char* deviceId) {
    decltype(midiOutputMap)::iterator it = midiOutputMap.find(deviceId);
    if (it != midiOutputMap.end()) {
        char midi[1] = {(char)0xfb};
        int status = snd_rawmidi_write(it->second, midi, 1);
        if (status < 0) {
            midiOutputMap.erase(deviceId);
            UnitySendMessage(GAME_OBJECT_NAME, "OnMidiOutputDeviceDetached", deviceId);
        }
    }
}

void SendMidiStop(const char* deviceId) {
    decltype(midiOutputMap)::iterator it = midiOutputMap.find(deviceId);
    if (it != midiOutputMap.end()) {
        char midi[1] = {(char)0xfc};
        int status = snd_rawmidi_write(it->second, midi, 1);
        if (status < 0) {
            midiOutputMap.erase(deviceId);
            UnitySendMessage(GAME_OBJECT_NAME, "OnMidiOutputDeviceDetached", deviceId);
        }
    }
}

void SendMidiActiveSensing(const char* deviceId) {
    decltype(midiOutputMap)::iterator it = midiOutputMap.find(deviceId);
    if (it != midiOutputMap.end()) {
        char midi[1] = {(char)0xfe};
        int status = snd_rawmidi_write(it->second, midi, 1);
        if (status < 0) {
            midiOutputMap.erase(deviceId);
            UnitySendMessage(GAME_OBJECT_NAME, "OnMidiOutputDeviceDetached", deviceId);
        }
    }
}

void SendMidiReset(const char* deviceId) {
    decltype(midiOutputMap)::iterator it = midiOutputMap.find(deviceId);
    if (it != midiOutputMap.end()) {
        char midi[1] = {(char)0xff};
        int status = snd_rawmidi_write(it->second, midi, 1);
        if (status < 0) {
            midiOutputMap.erase(deviceId);
            UnitySendMessage(GAME_OBJECT_NAME, "OnMidiOutputDeviceDetached", deviceId);
        }
    }
}

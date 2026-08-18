#include <algorithm>
#include <cerrno>
#include <chrono>
#include <iterator>
#include <map>
#include <mutex>
#include <poll.h>
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
void InitializeMidi2Linux();
void TerminateMidiLinux();
void TerminateMidi2Linux();

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

void SendUmpMessage(const char* deviceId, uint32_t* ump, int length);

#ifdef __cplusplus
}
#endif

std::map<std::string, snd_rawmidi_t*> midiInputMap;
std::map<std::string, snd_rawmidi_t*> midiOutputMap;
std::map<std::string, snd_ump_t*> midi2InputMap;
std::map<std::string, snd_ump_t*> midi2OutputMap;
std::map<std::string, snd_seq_addr_t> virtualMidiInputMap;
std::map<std::string, snd_seq_addr_t> virtualMidiOutputMap;
std::map<std::string, snd_seq_addr_t> virtualMidi2InputMap;
std::map<std::string, snd_seq_addr_t> virtualMidi2OutputMap;
std::map<std::string, std::string> deviceNames;

std::mutex midiInputMapMutex;
std::mutex midiOutputMapMutex;
std::mutex midi2InputMapMutex;
std::mutex midi2OutputMapMutex;
std::mutex virtualMidiInputMapMutex;
std::mutex virtualMidiOutputMapMutex;
std::mutex virtualMidi2InputMapMutex;
std::mutex virtualMidi2OutputMapMutex;
std::mutex deviceNamesMutex;

struct SeqClient {
    snd_seq_t *handle;
    std::mutex mutex;
    int clientId;
    int portNumber;

    SeqClient() : handle(nullptr), clientId(-1), portNumber(-1) {}
};

SeqClient seqMidi1;
SeqClient seqMidi2;

const char *GAME_OBJECT_NAME = "MidiManager";
const unsigned int SEQ_PORT_CAPS_MIDI1 =
    SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ |
    SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE;
const unsigned int SEQ_PORT_CAPS_MIDI2 =
    SEQ_PORT_CAPS_MIDI1 | SND_SEQ_PORT_CAP_UMP_ENDPOINT;

volatile bool isStopped = true;
volatile bool isMidi1Enabled = false;
volatile bool isMidi2Enabled = false;

OnSendMessageDelegate onSendMessage;

void UnitySendMessage(const char* obj, const char* method, const char* msg) {
    if (onSendMessage) {
        onSendMessage(method, msg);
    }
}

static bool openSeqClient(SeqClient &client, const char *name, int midiVersion, unsigned int caps) {
    std::lock_guard<std::mutex> lock(client.mutex);
    if (client.handle != nullptr) {
        return true;
    }

    if (snd_seq_open(&client.handle, "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK) < 0) {
        client.handle = nullptr;
        return false;
    }

    snd_seq_set_client_name(client.handle, name);
    if (midiVersion != SND_SEQ_CLIENT_LEGACY_MIDI) {
        snd_seq_set_client_midi_version(client.handle, midiVersion);
    }

    client.clientId = snd_seq_client_id(client.handle);
    client.portNumber = snd_seq_create_simple_port(client.handle, "inout", caps, SND_SEQ_PORT_TYPE_APPLICATION);
    if (client.portNumber < 0) {
        snd_seq_close(client.handle);
        client.handle = nullptr;
        client.clientId = -1;
        return false;
    }

    return true;
}

static void closeSeqClient(SeqClient &client) {
    std::lock_guard<std::mutex> lock(client.mutex);
    if (client.handle != nullptr) {
        snd_seq_close(client.handle);
        client.handle = nullptr;
        client.clientId = -1;
        client.portNumber = -1;
    }
}

static void unsubscribeSeqInput(SeqClient &client, const snd_seq_addr_t &addr);

static const int kPollTimeoutMs = 100;

static bool pollSeqClient(SeqClient &client) {
    struct pollfd pfds[8];
    int npfds = 0;
    {
        std::lock_guard<std::mutex> lock(client.mutex);
        if (client.handle == nullptr) {
            return false;
        }
        npfds = snd_seq_poll_descriptors_count(client.handle, POLLIN);
        if (npfds > 8) {
            npfds = 8;
        }
        if (npfds > 0) {
            snd_seq_poll_descriptors(client.handle, pfds, npfds, POLLIN);
        }
    }
    if (npfds <= 0) {
        return false;
    }
    return poll(pfds, npfds, kPollTimeoutMs) > 0;
}

static snd_rawmidi_t *stealRawmidi(std::map<std::string, snd_rawmidi_t*> &devices, std::mutex &mutex, const std::string &id) {
    std::lock_guard<std::mutex> lock(mutex);
    std::map<std::string, snd_rawmidi_t*>::iterator it = devices.find(id);
    if (it == devices.end()) {
        return nullptr;
    }
    snd_rawmidi_t *handle = it->second;
    devices.erase(it);
    return handle;
}

static snd_ump_t *stealUmp(std::map<std::string, snd_ump_t*> &devices, std::mutex &mutex, const std::string &id) {
    std::lock_guard<std::mutex> lock(mutex);
    std::map<std::string, snd_ump_t*>::iterator it = devices.find(id);
    if (it == devices.end()) {
        return nullptr;
    }
    snd_ump_t *handle = it->second;
    devices.erase(it);
    return handle;
}

static void closeRawmidiAndNotify(std::map<std::string, snd_rawmidi_t*> &devices, std::mutex &mutex,
                                  const std::string &id, const char *detachMethod) {
    snd_rawmidi_t *handle = stealRawmidi(devices, mutex, id);
    if (handle != nullptr) {
        snd_rawmidi_close(handle);
        UnitySendMessage(GAME_OBJECT_NAME, detachMethod, id.c_str());
    }
}

static void closeUmpAndNotify(std::map<std::string, snd_ump_t*> &devices, std::mutex &mutex,
                              const std::string &id, const char *detachMethod) {
    snd_ump_t *handle = stealUmp(devices, mutex, id);
    if (handle != nullptr) {
        snd_ump_close(handle);
        UnitySendMessage(GAME_OBJECT_NAME, detachMethod, id.c_str());
    }
}

static void closeAllRawmidiAndNotify(std::map<std::string, snd_rawmidi_t*> &devices, std::mutex &mutex,
                                     const char *detachMethod) {
    std::map<std::string, snd_rawmidi_t*> stolen;
    {
        std::lock_guard<std::mutex> lock(mutex);
        stolen.swap(devices);
    }
    for (std::map<std::string, snd_rawmidi_t*>::iterator it = stolen.begin(); it != stolen.end(); ++it) {
        if (it->second != nullptr) {
            snd_rawmidi_close(it->second);
        }
        UnitySendMessage(GAME_OBJECT_NAME, detachMethod, it->first.c_str());
    }
}

static void closeAllUmpAndNotify(std::map<std::string, snd_ump_t*> &devices, std::mutex &mutex,
                                 const char *detachMethod) {
    std::map<std::string, snd_ump_t*> stolen;
    {
        std::lock_guard<std::mutex> lock(mutex);
        stolen.swap(devices);
    }
    for (std::map<std::string, snd_ump_t*>::iterator it = stolen.begin(); it != stolen.end(); ++it) {
        if (it->second != nullptr) {
            snd_ump_close(it->second);
        }
        UnitySendMessage(GAME_OBJECT_NAME, detachMethod, it->first.c_str());
    }
}

static void clearVirtualSeqMap(std::map<std::string, snd_seq_addr_t> &devices, std::mutex &mutex,
                               SeqClient *seqClient, const char *detachMethod) {
    std::map<std::string, snd_seq_addr_t> stolen;
    {
        std::lock_guard<std::mutex> lock(mutex);
        stolen.swap(devices);
    }
    for (std::map<std::string, snd_seq_addr_t>::iterator it = stolen.begin(); it != stolen.end(); ++it) {
        if (seqClient != nullptr) {
            unsubscribeSeqInput(*seqClient, it->second);
        }
        UnitySendMessage(GAME_OBJECT_NAME, detachMethod, it->first.c_str());
    }
}

static void shutdownMidi1Resources() {
    clearVirtualSeqMap(virtualMidiInputMap, virtualMidiInputMapMutex, &seqMidi1, "OnMidiInputDeviceDetached");
    clearVirtualSeqMap(virtualMidiOutputMap, virtualMidiOutputMapMutex, nullptr, "OnMidiOutputDeviceDetached");
    closeAllRawmidiAndNotify(midiInputMap, midiInputMapMutex, "OnMidiInputDeviceDetached");
    closeAllRawmidiAndNotify(midiOutputMap, midiOutputMapMutex, "OnMidiOutputDeviceDetached");
    closeSeqClient(seqMidi1);
}

static void shutdownMidi2Resources() {
    clearVirtualSeqMap(virtualMidi2InputMap, virtualMidi2InputMapMutex, &seqMidi2, "OnMidi2InputDeviceDetached");
    clearVirtualSeqMap(virtualMidi2OutputMap, virtualMidi2OutputMapMutex, nullptr, "OnMidi2OutputDeviceDetached");
    closeAllUmpAndNotify(midi2InputMap, midi2InputMapMutex, "OnMidi2InputDeviceDetached");
    closeAllUmpAndNotify(midi2OutputMap, midi2OutputMapMutex, "OnMidi2OutputDeviceDetached");
    closeSeqClient(seqMidi2);
}

static bool subscribeSeqInput(SeqClient &client, const snd_seq_addr_t &addr) {
    std::lock_guard<std::mutex> lock(client.mutex);
    if (client.handle == nullptr || client.portNumber < 0) {
        return false;
    }
    int err = snd_seq_connect_from(client.handle, client.portNumber, addr.client, addr.port);
    // already subscribed is not a failure; the port still delivers events to us
    return err >= 0 || err == -EBUSY;
}

static void unsubscribeSeqInput(SeqClient &client, const snd_seq_addr_t &addr) {
    std::lock_guard<std::mutex> lock(client.mutex);
    if (client.handle == nullptr || client.portNumber < 0) {
        return;
    }
    snd_seq_disconnect_from(client.handle, client.portNumber, addr.client, addr.port);
}

static SeqClient *selectQuerySeq() {
    {
        std::lock_guard<std::mutex> lock(seqMidi1.mutex);
        if (seqMidi1.handle != nullptr) {
            return &seqMidi1;
        }
    }
    {
        std::lock_guard<std::mutex> lock(seqMidi2.mutex);
        if (seqMidi2.handle != nullptr) {
            return &seqMidi2;
        }
    }
    return nullptr;
}

static int seqQueryNextClient(SeqClient &client, snd_seq_client_info_t *cinfo) {
    std::lock_guard<std::mutex> lock(client.mutex);
    if (client.handle == nullptr) {
        return -ENODEV;
    }
    return snd_seq_query_next_client(client.handle, cinfo);
}

static int seqQueryNextPort(SeqClient &client, snd_seq_port_info_t *pinfo) {
    std::lock_guard<std::mutex> lock(client.mutex);
    if (client.handle == nullptr) {
        return -ENODEV;
    }
    return snd_seq_query_next_port(client.handle, pinfo);
}

static void initSeqEventDirect(snd_seq_event_t *ev, int sourcePort, const snd_seq_addr_t &dest) {
    snd_seq_ev_clear(ev);
    snd_seq_ev_set_source(ev, sourcePort);
    snd_seq_ev_set_dest(ev, dest.client, dest.port);
    snd_seq_ev_set_direct(ev);
}

static void initSeqUmpEventDirect(snd_seq_ump_event_t *ev, int sourcePort, const snd_seq_addr_t &dest) {
    snd_seq_ump_ev_clear(ev);
    snd_seq_ev_set_source(ev, sourcePort);
    snd_seq_ev_set_dest(ev, dest.client, dest.port);
    snd_seq_ev_set_direct(ev);
}

static void outputSeqEvent(SeqClient &client, snd_seq_event_t *ev) {
    std::lock_guard<std::mutex> lock(client.mutex);
    if (client.handle == nullptr) {
        return;
    }
    snd_seq_event_output(client.handle, ev);
    snd_seq_drain_output(client.handle);
}

static void outputSeqUmpEvent(SeqClient &client, snd_seq_ump_event_t *ev) {
    std::lock_guard<std::mutex> lock(client.mutex);
    if (client.handle == nullptr) {
        return;
    }
    snd_seq_ump_event_output_direct(client.handle, ev);
    snd_seq_drain_output(client.handle);
}

static void sendSeqRealtimeEvent(const snd_seq_addr_t &dest, snd_seq_event_type_t type) {
    snd_seq_event_t ev;
    initSeqEventDirect(&ev, seqMidi1.portNumber, dest);
    ev.type = type;
    snd_seq_ev_set_fixed(&ev);
    outputSeqEvent(seqMidi1, &ev);
}

// UMP packet size in 32-bit words, from the message type in bits 31-28 of the first word.
static int umpPacketWordCount(uint32_t firstWord) {
    static const int kWords[16] = {
        1, 1, 1, 2, 2, 4, 1, 1, 2, 2, 2, 3, 4, 4, 4, 4
    };
    return kWords[(firstWord >> 28) & 0x0f];
}

static void sendUmpMessageToUnity(const char *deviceId, const uint32_t *ump, int wordCount) {
    if (deviceId == nullptr || ump == nullptr || wordCount <= 0) {
        return;
    }
    std::ostringstream oss;
    oss << deviceId;
    for (int i = 0; i < wordCount; ++i) {
        oss << "," << ump[i];
    }
    UnitySendMessage(GAME_OBJECT_NAME, "OnUmpMessage", oss.str().c_str());
}

static void dispatchUmpWords(const char *deviceId, const uint32_t *ump, int wordCount) {
    int offset = 0;
    while (offset < wordCount) {
        int packetWords = umpPacketWordCount(ump[offset]);
        if (offset + packetWords > wordCount) {
            packetWords = wordCount - offset;
        }
        sendUmpMessageToUnity(deviceId, ump + offset, packetWords);
        offset += packetWords;
    }
}

void virtualMidiEventWatcher() {
    using namespace std::chrono_literals;
    char deviceId[32];
    char eventMessage[128];

    while (!isStopped) {
        if (!isMidi1Enabled) {
            std::this_thread::sleep_for(10ms);
            continue;
        }
        if (!pollSeqClient(seqMidi1)) {
            continue;
        }

        snd_seq_event_t ev;
        std::vector<unsigned char> sysex;
        bool haveEvent = false;
        {
            std::lock_guard<std::mutex> lock(seqMidi1.mutex);
            if (seqMidi1.handle != nullptr) {
                snd_seq_event_t *rawEv = nullptr;
                if (snd_seq_event_input(seqMidi1.handle, &rawEv) >= 0 && rawEv != nullptr) {
                    ev = *rawEv;
                    haveEvent = true;
                    if (rawEv->type == SND_SEQ_EVENT_SYSEX && rawEv->data.ext.ptr != nullptr && rawEv->data.ext.len > 0) {
                        const unsigned char *ptr = static_cast<const unsigned char *>(rawEv->data.ext.ptr);
                        sysex.assign(ptr, ptr + rawEv->data.ext.len);
                    }
                }
            }
        }

        if (!haveEvent) {
            continue;
        }

        sprintf(deviceId, "seq:%d-%d", ev.source.client, ev.source.port);
        {
            std::lock_guard<std::mutex> lock(virtualMidiInputMapMutex);
            if (virtualMidiInputMap.find(deviceId) == virtualMidiInputMap.end()) {
                continue;
            }
        }

        // https://www.alsa-project.org/alsa-doc/alsa-lib/group___seq_events.html#gaef39e1f267006faf7abc91c3cb32ea40
        switch (ev.type) {
            case SND_SEQ_EVENT_NOTEON:
                sprintf(eventMessage, "%s,0,%d,%d,%d", deviceId, ev.data.note.channel, ev.data.note.note, ev.data.note.velocity);
                UnitySendMessage(GAME_OBJECT_NAME, "OnMidiNoteOn", eventMessage);
                break;
            case SND_SEQ_EVENT_NOTEOFF:
                sprintf(eventMessage, "%s,0,%d,%d,%d", deviceId, ev.data.note.channel, ev.data.note.note, ev.data.note.velocity);
                UnitySendMessage(GAME_OBJECT_NAME, "OnMidiNoteOff", eventMessage);
                break;
            case SND_SEQ_EVENT_CONTROLLER:
                sprintf(eventMessage, "%s,0,%d,%d,%d", deviceId, ev.data.control.channel, ev.data.control.param, ev.data.control.value);
                UnitySendMessage(GAME_OBJECT_NAME, "OnMidiControlChange", eventMessage);
                break;
            case SND_SEQ_EVENT_PGMCHANGE:
                sprintf(eventMessage, "%s,0,%d,%d", deviceId, ev.data.control.channel, ev.data.control.value);
                UnitySendMessage(GAME_OBJECT_NAME, "OnMidiProgramChange", eventMessage);
                break;
            case SND_SEQ_EVENT_CHANPRESS:
                sprintf(eventMessage, "%s,0,%d,%d", deviceId, ev.data.control.channel, ev.data.control.value);
                UnitySendMessage(GAME_OBJECT_NAME, "OnMidiChannelAftertouch", eventMessage);
                break;
            case SND_SEQ_EVENT_KEYPRESS:
                sprintf(eventMessage, "%s,0,%d,%d,%d", deviceId, ev.data.note.channel, ev.data.note.note, ev.data.note.velocity);
                UnitySendMessage(GAME_OBJECT_NAME, "OnMidiPolyphonicAftertouch", eventMessage);
                break;
            case SND_SEQ_EVENT_PITCHBEND:
                sprintf(eventMessage, "%s,0,%d,%d", deviceId, ev.data.control.channel, ev.data.control.value + 8192);
                UnitySendMessage(GAME_OBJECT_NAME, "OnMidiPitchWheel", eventMessage);
                break;
            case SND_SEQ_EVENT_SYSEX:
                {
                    std::ostringstream oss;
                    oss << deviceId;
                    oss << ",0,";
                    std::copy(sysex.begin(), sysex.end(), std::ostream_iterator<int>(oss, ","));
                    UnitySendMessage(GAME_OBJECT_NAME, "OnMidiSystemExclusive", oss.str().c_str());
                }
                break;
            case SND_SEQ_EVENT_SONGPOS:
                sprintf(eventMessage, "%s,0,%d", deviceId, ev.data.control.value);
                UnitySendMessage(GAME_OBJECT_NAME, "OnMidiSongPositionPointer", eventMessage);
                break;
            case SND_SEQ_EVENT_SONGSEL:
                sprintf(eventMessage, "%s,0,%d", deviceId, ev.data.control.value);
                UnitySendMessage(GAME_OBJECT_NAME, "OnMidiSongSelect", eventMessage);
                break;
            case SND_SEQ_EVENT_QFRAME:
                sprintf(eventMessage, "%s,0,%d", deviceId, ev.data.control.value);
                UnitySendMessage(GAME_OBJECT_NAME, "OnMidiTimeCodeQuarterFrame", eventMessage);
                break;
            case SND_SEQ_EVENT_TUNE_REQUEST:
                sprintf(eventMessage, "%s,0", deviceId);
                UnitySendMessage(GAME_OBJECT_NAME, "OnMidiTuneRequest", eventMessage);
                break;
            case SND_SEQ_EVENT_CLOCK:
                sprintf(eventMessage, "%s,0", deviceId);
                UnitySendMessage(GAME_OBJECT_NAME, "OnMidiTimingClock", eventMessage);
                break;
            case SND_SEQ_EVENT_START:
                sprintf(eventMessage, "%s,0", deviceId);
                UnitySendMessage(GAME_OBJECT_NAME, "OnMidiStart", eventMessage);
                break;
            case SND_SEQ_EVENT_CONTINUE:
                sprintf(eventMessage, "%s,0", deviceId);
                UnitySendMessage(GAME_OBJECT_NAME, "OnMidiContinue", eventMessage);
                break;
            case SND_SEQ_EVENT_STOP:
                sprintf(eventMessage, "%s,0", deviceId);
                UnitySendMessage(GAME_OBJECT_NAME, "OnMidiStop", eventMessage);
                break;
            case SND_SEQ_EVENT_SENSING:
                sprintf(eventMessage, "%s,0", deviceId);
                UnitySendMessage(GAME_OBJECT_NAME, "OnMidiActiveSensing", eventMessage);
                break;
            case SND_SEQ_EVENT_RESET:
                sprintf(eventMessage, "%s,0", deviceId);
                UnitySendMessage(GAME_OBJECT_NAME, "OnMidiReset", eventMessage);
                break;
        }
    }
}

void virtualMidi2EventWatcher() {
    using namespace std::chrono_literals;
    char deviceId[32];

    while (!isStopped) {
        if (!isMidi2Enabled) {
            std::this_thread::sleep_for(10ms);
            continue;
        }
        if (!pollSeqClient(seqMidi2)) {
            continue;
        }

        snd_seq_ump_event_t ev;
        bool haveEvent = false;
        {
            std::lock_guard<std::mutex> lock(seqMidi2.mutex);
            if (seqMidi2.handle != nullptr) {
                snd_seq_ump_event_t *rawEv = nullptr;
                if (snd_seq_ump_event_input(seqMidi2.handle, &rawEv) >= 0 && rawEv != nullptr) {
                    ev = *rawEv;
                    haveEvent = true;
                }
            }
        }

        if (!haveEvent) {
            continue;
        }

        sprintf(deviceId, "seq:%d-%d", ev.source.client, ev.source.port);
        {
            std::lock_guard<std::mutex> lock(virtualMidi2InputMapMutex);
            if (virtualMidi2InputMap.find(deviceId) == virtualMidi2InputMap.end()) {
                continue;
            }
        }

        // UMP clients set SND_SEQ_EVENT_UMP in flags and leave type as 0
        if ((ev.flags & SND_SEQ_EVENT_UMP) != 0 || ev.type == SND_SEQ_EVENT_UMP) {
            dispatchUmpWords(deviceId, ev.ump, umpPacketWordCount(ev.ump[0]));
        }
    }
}

void midiEventWatcher(std::string deviceIdStr, snd_rawmidi_t* midiInput) {
    using namespace std::chrono_literals;
    ssize_t nread;
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
        bool tracked = false;
        {
            std::lock_guard<std::mutex> lock(midiInputMapMutex);
            std::map<std::string, snd_rawmidi_t*>::iterator it = midiInputMap.find(deviceIdStr);
            tracked = it != midiInputMap.end() && it->second == midiInput;
        }
        if (!tracked) {
            break;
        }
        if (!isMidi1Enabled) {
            std::this_thread::sleep_for(10ms);
            continue;
        }

        struct pollfd pfds[8];
        int npfd = 0;
        {
            std::lock_guard<std::mutex> lock(midiInputMapMutex);
            std::map<std::string, snd_rawmidi_t*>::iterator it = midiInputMap.find(deviceIdStr);
            if (it == midiInputMap.end() || it->second != midiInput) {
                break;
            }
            npfd = snd_rawmidi_poll_descriptors_count(midiInput);
            if (npfd > 8) {
                npfd = 8;
            }
            if (npfd > 0) {
                snd_rawmidi_poll_descriptors(midiInput, pfds, npfd);
            }
        }
        if (npfd <= 0) {
            std::this_thread::sleep_for(10ms);
            continue;
        }

        int pret = poll(pfds, npfd, kPollTimeoutMs);
        if (pret < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (pret == 0) {
            continue;
        }

        nread = 0;
        bool closedByError = false;
        {
            std::lock_guard<std::mutex> lock(midiInputMapMutex);
            std::map<std::string, snd_rawmidi_t*>::iterator it = midiInputMap.find(deviceIdStr);
            if (it == midiInputMap.end() || it->second != midiInput) {
                break;
            }
            unsigned short revents = 0;
            if (snd_rawmidi_poll_descriptors_revents(midiInput, pfds, npfd, &revents) < 0 ||
                (revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                midiInputMap.erase(it);
                snd_rawmidi_close(midiInput);
                closedByError = true;
            } else if ((revents & POLLIN) != 0) {
                nread = snd_rawmidi_read(midiInput, buffer, sizeof(buffer));
            }
        }
        if (closedByError) {
            UnitySendMessage(GAME_OBJECT_NAME, "OnMidiInputDeviceDetached", deviceId);
            break;
        }
        if (nread == -EAGAIN) {
            continue;
        }
        if (nread < 0) {
            closeRawmidiAndNotify(midiInputMap, midiInputMapMutex, deviceIdStr, "OnMidiInputDeviceDetached");
            break;
        }

        if (nread > 0) {
            // parse MIDI
            for (int i = 0; i < nread; i++) {
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
                                    sprintf(eventMessage, "%s,0", deviceId);
                                    UnitySendMessage(GAME_OBJECT_NAME, "OnMidiTuneRequest", eventMessage);
                                    midiState = MIDI_STATE_WAIT;
                                    break;
                                case 0xf8:
                                    // 0xf8 Timing Clock : 1byte
                                    sprintf(eventMessage, "%s,0", deviceId);
                                    UnitySendMessage(GAME_OBJECT_NAME, "OnMidiTimingClock", eventMessage);
                                    midiState = MIDI_STATE_WAIT;
                                    break;
                                case 0xfa:
                                    // 0xfa Start : 1byte
                                    sprintf(eventMessage, "%s,0", deviceId);
                                    UnitySendMessage(GAME_OBJECT_NAME, "OnMidiStart", eventMessage);
                                    midiState = MIDI_STATE_WAIT;
                                    break;
                                case 0xfb:
                                    // 0xfb Continue : 1byte
                                    sprintf(eventMessage, "%s,0", deviceId);
                                    UnitySendMessage(GAME_OBJECT_NAME, "OnMidiContinue", eventMessage);
                                    midiState = MIDI_STATE_WAIT;
                                    break;
                                case 0xfc:
                                    // 0xfc Stop : 1byte
                                    sprintf(eventMessage, "%s,0", deviceId);
                                    UnitySendMessage(GAME_OBJECT_NAME, "OnMidiStop", eventMessage);
                                    midiState = MIDI_STATE_WAIT;
                                    break;
                                case 0xfe:
                                    // 0xfe Active Sensing : 1byte
                                    sprintf(eventMessage, "%s,0", deviceId);
                                    UnitySendMessage(GAME_OBJECT_NAME, "OnMidiActiveSensing", eventMessage);
                                    midiState = MIDI_STATE_WAIT;
                                    break;
                                case 0xff:
                                    // 0xff Reset : 1byte
                                    sprintf(eventMessage, "%s,0", deviceId);
                                    UnitySendMessage(GAME_OBJECT_NAME, "OnMidiReset", eventMessage);
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
    }
}

void midi2EventWatcher(std::string deviceIdStr, snd_ump_t* midiInput) {
    using namespace std::chrono_literals;
    uint32_t buffer[1024];

    while (!isStopped) {
        bool tracked = false;
        {
            std::lock_guard<std::mutex> lock(midi2InputMapMutex);
            std::map<std::string, snd_ump_t*>::iterator it = midi2InputMap.find(deviceIdStr);
            tracked = it != midi2InputMap.end() && it->second == midiInput;
        }
        if (!tracked) {
            break;
        }
        if (!isMidi2Enabled) {
            std::this_thread::sleep_for(10ms);
            continue;
        }

        struct pollfd pfds[8];
        int npfd = 0;
        {
            std::lock_guard<std::mutex> lock(midi2InputMapMutex);
            std::map<std::string, snd_ump_t*>::iterator it = midi2InputMap.find(deviceIdStr);
            if (it == midi2InputMap.end() || it->second != midiInput) {
                break;
            }
            npfd = snd_ump_poll_descriptors_count(midiInput);
            if (npfd > 8) {
                npfd = 8;
            }
            if (npfd > 0) {
                snd_ump_poll_descriptors(midiInput, pfds, npfd);
            }
        }
        if (npfd <= 0) {
            std::this_thread::sleep_for(10ms);
            continue;
        }

        int pret = poll(pfds, npfd, kPollTimeoutMs);
        if (pret < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (pret == 0) {
            continue;
        }

        ssize_t nread = 0;
        bool closedByError = false;
        {
            std::lock_guard<std::mutex> lock(midi2InputMapMutex);
            std::map<std::string, snd_ump_t*>::iterator it = midi2InputMap.find(deviceIdStr);
            if (it == midi2InputMap.end() || it->second != midiInput) {
                break;
            }
            unsigned short revents = 0;
            if (snd_ump_poll_descriptors_revents(midiInput, pfds, npfd, &revents) < 0 ||
                (revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                midi2InputMap.erase(it);
                snd_ump_close(midiInput);
                closedByError = true;
            } else if ((revents & POLLIN) != 0) {
                nread = snd_ump_read(midiInput, buffer, sizeof(buffer));
            }
        }
        if (closedByError) {
            UnitySendMessage(GAME_OBJECT_NAME, "OnMidi2InputDeviceDetached", deviceIdStr.c_str());
            break;
        }
        if (nread == -EAGAIN) {
            continue;
        }
        if (nread < 0) {
            closeUmpAndNotify(midi2InputMap, midi2InputMapMutex, deviceIdStr, "OnMidi2InputDeviceDetached");
            break;
        }

        if (nread > 0) {
            ssize_t wordCount = nread / static_cast<ssize_t>(sizeof(uint32_t));
            dispatchUmpWords(deviceIdStr.c_str(), buffer, static_cast<int>(wordCount));
        }
    }
}

#define LIST_INPUT    1
#define LIST_OUTPUT    2
#define perm_ok(cap,bits) (((cap) & (bits)) == (bits))
static int check_permission(snd_seq_port_info_t *pinfo, int perm) {
    int cap = snd_seq_port_info_get_capability(pinfo);

    if (cap & SND_SEQ_PORT_CAP_NO_EXPORT) {
        return 0;
    }

    if (perm & LIST_INPUT) {
        if (perm_ok(cap, SND_SEQ_PORT_CAP_READ)) {
            return 1;
        }
        if (perm_ok(cap, SND_SEQ_PORT_CAP_SUBS_READ)) {
            return 1;
        }
    }

    if (perm & LIST_OUTPUT) {
        if (perm_ok(cap, SND_SEQ_PORT_CAP_WRITE)) {
            return 1;
        }
        if (perm_ok(cap, SND_SEQ_PORT_CAP_SUBS_WRITE)) {
            return 1;
        }
    }

    return 0;
}

static int rawmidiSubdeviceCount(snd_ctl_t *ctl, snd_rawmidi_info_t *info, int device, snd_rawmidi_stream_t stream) {
    snd_rawmidi_info_set_device(info, device);
    snd_rawmidi_info_set_stream(info, stream);
    snd_rawmidi_info_set_subdevice(info, 0);
    if (snd_ctl_rawmidi_info(ctl, info) < 0) {
        return 0;
    }
    return static_cast<int>(snd_rawmidi_info_get_subdevices_count(info));
}

static std::string rawmidiSubdeviceName(snd_ctl_t *ctl, snd_rawmidi_info_t *info, int device,
                                       snd_rawmidi_stream_t stream, int sub, const char *fallbackName) {
    snd_rawmidi_info_set_device(info, device);
    snd_rawmidi_info_set_stream(info, stream);
    snd_rawmidi_info_set_subdevice(info, sub);
    if (snd_ctl_rawmidi_info(ctl, info) < 0) {
        return fallbackName != nullptr ? fallbackName : "";
    }

    const char *subName = snd_rawmidi_info_get_subdevice_name(info);
    if (subName != nullptr && subName[0] != '\0') {
        return subName;
    }

    const char *devName = snd_rawmidi_info_get_name(info);
    if (devName != nullptr && devName[0] != '\0') {
        return devName;
    }

    return fallbackName != nullptr ? fallbackName : "";
}

static std::string hwCardDeviceKey(int card, int device) {
    char key[32];
    sprintf(key, "%d:%d", card, device);
    return key;
}

static bool fillRawmidiInfo(snd_ctl_t *ctl, snd_rawmidi_info_t *info, int device) {
    snd_rawmidi_info_set_device(info, device);
    snd_rawmidi_info_set_subdevice(info, 0);
    snd_rawmidi_info_set_stream(info, SND_RAWMIDI_STREAM_INPUT);
    if (snd_ctl_rawmidi_info(ctl, info) >= 0) {
        return true;
    }
    snd_rawmidi_info_set_stream(info, SND_RAWMIDI_STREAM_OUTPUT);
    return snd_ctl_rawmidi_info(ctl, info) >= 0;
}

static void recordUmpOccupiedDevices(snd_ctl_t *ctl, snd_rawmidi_info_t *info, int card, int umpDevice,
                                     std::set<std::string> *occupied) {
    occupied->insert(hwCardDeviceKey(card, umpDevice));
    if (!fillRawmidiInfo(ctl, info, umpDevice)) {
        return;
    }
    int tied = snd_rawmidi_info_get_tied_device(info);
    if (tied >= 0) {
        occupied->insert(hwCardDeviceKey(card, tied));
    }
}

static bool shouldSkipMidi1Rawmidi(snd_ctl_t *ctl, snd_rawmidi_info_t *info, int card, int device,
                                  bool midi2Enabled, const std::set<std::string> &umpOccupied) {
    if (!fillRawmidiInfo(ctl, info, device)) {
        return false;
    }
#ifdef SND_RAWMIDI_INFO_UMP
    if (snd_rawmidi_info_get_flags(info) & SND_RAWMIDI_INFO_UMP) {
        return true;
    }
#endif
    if (!midi2Enabled) {
        return false;
    }
    if (umpOccupied.find(hwCardDeviceKey(card, device)) != umpOccupied.end()) {
        return true;
    }
    return snd_rawmidi_info_get_tied_device(info) >= 0;
}

void midiConnectionWatcher() {
    using namespace std::chrono_literals;

    char deviceId[32];

    // virtual midi
    int client;
    int port;

    snd_seq_client_info_t *cinfo;
    snd_seq_port_info_t *pinfo;

    snd_seq_client_info_alloca(&cinfo);
    snd_seq_port_info_alloca(&pinfo);

    int midi_version;

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

    // ump_endpoint
    snd_ump_endpoint_info_t *umpinfo;
    snd_ump_endpoint_info_alloca(&umpinfo);

    // current connections to detect detached
    std::set<std::string> currentConnections;
    std::set<std::string> connectionsToRemove;
    std::set<std::string> umpOccupiedHw;

    while (!isStopped) {
        // virtual midi
        currentConnections.clear();
        umpOccupiedHw.clear();
        SeqClient *querySeq = selectQuerySeq();
        if (querySeq != nullptr) {
        snd_seq_client_info_set_client(cinfo, -1);
        while (seqQueryNextClient(*querySeq, cinfo) >= 0) {
            // loop with client
            if (snd_seq_client_info_get_type(cinfo) == SND_SEQ_KERNEL_CLIENT) {
                // system client: ignore
                continue;
            }

            // reset query info
            snd_seq_port_info_set_client(pinfo, snd_seq_client_info_get_client(cinfo));
            snd_seq_port_info_set_port(pinfo, -1);

            while (seqQueryNextPort(*querySeq, pinfo) >= 0) {
                // loop with port
                snd_seq_addr_t addr;
                addr.client = snd_seq_client_info_get_client(cinfo);
                addr.port = snd_seq_port_info_get_port(pinfo);

                midi_version = snd_seq_client_info_get_midi_version(cinfo);

                if (addr.client == seqMidi1.clientId || addr.client == seqMidi2.clientId) {
                    // self client: ignore
                    continue;
                }

                if (check_permission(pinfo, LIST_INPUT)) {
                    // found a input port
                    sprintf(deviceId, "seq:%d-%d", addr.client, addr.port);
                    currentConnections.insert(deviceId);

                    if (midi_version == 0) {
                        if (isMidi1Enabled) {
                            std::lock_guard<std::mutex> lock(virtualMidiInputMapMutex);
                            if (virtualMidiInputMap.find(deviceId) == virtualMidiInputMap.end() && subscribeSeqInput(seqMidi1, addr)) {
                                const char* deviceName = snd_seq_client_info_get_name(cinfo);
                                if (deviceNames.find(deviceId) == deviceNames.end()) {
                                    deviceNames.insert(std::make_pair(deviceId, deviceName));
                                }
                                virtualMidiInputMap.insert(std::make_pair(deviceId, addr));
    
                                UnitySendMessage(GAME_OBJECT_NAME, "OnMidiInputDeviceAttached", deviceId);
                            }
                        }
                    } else if (midi_version == 1 || midi_version == 2) {
                        if (isMidi2Enabled) {
                            std::lock_guard<std::mutex> lock(virtualMidi2InputMapMutex);
                            if (virtualMidi2InputMap.find(deviceId) == virtualMidi2InputMap.end() && subscribeSeqInput(seqMidi2, addr)) {
                                const char* deviceName = snd_seq_client_info_get_name(cinfo);
                                if (deviceNames.find(deviceId) == deviceNames.end()) {
                                    deviceNames.insert(std::make_pair(deviceId, deviceName));
                                }
                                virtualMidi2InputMap.insert(std::make_pair(deviceId, addr));

                                UnitySendMessage(GAME_OBJECT_NAME, "OnMidi2InputDeviceAttached", deviceId);
                            }
                        }
                    }
                }

                if (check_permission(pinfo, LIST_OUTPUT)) {
                    // found a output port
                    sprintf(deviceId, "seq:%d-%d", addr.client, addr.port);
                    currentConnections.insert(deviceId);

                    if (midi_version == 0) {
                        if (isMidi1Enabled) {
                            std::lock_guard<std::mutex> lock(virtualMidiOutputMapMutex);
                            if (virtualMidiOutputMap.find(deviceId) == virtualMidiOutputMap.end()) {
                                const char* deviceName = snd_seq_client_info_get_name(cinfo);
                                if (deviceNames.find(deviceId) == deviceNames.end()) {
                                    deviceNames.insert(std::make_pair(deviceId, deviceName));
                                }
                                virtualMidiOutputMap.insert(std::make_pair(deviceId, addr));

                                UnitySendMessage(GAME_OBJECT_NAME, "OnMidiOutputDeviceAttached", deviceId);
                            }
                        }
                    } else if (midi_version == 1 || midi_version == 2) {
                        if (isMidi2Enabled) {
                            std::lock_guard<std::mutex> lock(virtualMidi2OutputMapMutex);
                            if (virtualMidi2OutputMap.find(deviceId) == virtualMidi2OutputMap.end()) {
                                const char* deviceName = snd_seq_client_info_get_name(cinfo);
                                if (deviceNames.find(deviceId) == deviceNames.end()) {
                                    deviceNames.insert(std::make_pair(deviceId, deviceName));
                                }
                                virtualMidi2OutputMap.insert(std::make_pair(deviceId, addr));

                                UnitySendMessage(GAME_OBJECT_NAME, "OnMidiOutput2DeviceAttached", deviceId);
                            }
                        }
                    }
                }
            }
        }

        {
            connectionsToRemove.clear();
            std::lock_guard<std::mutex> lock(virtualMidiInputMapMutex);

            for (std::map<std::string, snd_seq_addr_t>::iterator it = virtualMidiInputMap.begin(); it != virtualMidiInputMap.end(); ++it) {
                if (currentConnections.find(it->first) == currentConnections.end()) {
                    UnitySendMessage(GAME_OBJECT_NAME, "OnMidiInputDeviceDetached", it->first.c_str());
                    connectionsToRemove.insert(it->first);
                }
            }
            for (std::set<std::string>::iterator it = connectionsToRemove.begin(); it != connectionsToRemove.end(); ++it) {
                std::map<std::string, snd_seq_addr_t>::iterator found = virtualMidiInputMap.find(*it);
                if (found != virtualMidiInputMap.end()) {
                    unsubscribeSeqInput(seqMidi1, found->second);
                    virtualMidiInputMap.erase(found);
                }
            }
        }
        {
            connectionsToRemove.clear();
            std::lock_guard<std::mutex> lock(virtualMidiOutputMapMutex);
            for (std::map<std::string, snd_seq_addr_t>::iterator it = virtualMidiOutputMap.begin(); it != virtualMidiOutputMap.end(); ++it) {
                if (currentConnections.find(it->first) == currentConnections.end()) {
                    UnitySendMessage(GAME_OBJECT_NAME, "OnMidiOutputDeviceDetached", it->first.c_str());
                    connectionsToRemove.insert(it->first);
                }
            }
            for (std::set<std::string>::iterator it = connectionsToRemove.begin(); it != connectionsToRemove.end(); ++it) {
                virtualMidiOutputMap.erase(*it);
            }
        }
        {
            connectionsToRemove.clear();
            std::lock_guard<std::mutex> lock(virtualMidi2InputMapMutex);

            for (std::map<std::string, snd_seq_addr_t>::iterator it = virtualMidi2InputMap.begin(); it != virtualMidi2InputMap.end(); ++it) {
                if (currentConnections.find(it->first) == currentConnections.end()) {
                    UnitySendMessage(GAME_OBJECT_NAME, "OnMidi2InputDeviceDetached", it->first.c_str());
                    connectionsToRemove.insert(it->first);
                }
            }
            for (std::set<std::string>::iterator it = connectionsToRemove.begin(); it != connectionsToRemove.end(); ++it) {
                std::map<std::string, snd_seq_addr_t>::iterator found = virtualMidi2InputMap.find(*it);
                if (found != virtualMidi2InputMap.end()) {
                    unsubscribeSeqInput(seqMidi2, found->second);
                    virtualMidi2InputMap.erase(found);
                }
            }
        }
        {
            connectionsToRemove.clear();
            std::lock_guard<std::mutex> lock(virtualMidi2OutputMapMutex);
            for (std::map<std::string, snd_seq_addr_t>::iterator it = virtualMidi2OutputMap.begin(); it != virtualMidi2OutputMap.end(); ++it) {
                if (currentConnections.find(it->first) == currentConnections.end()) {
                    UnitySendMessage(GAME_OBJECT_NAME, "OnMidiOutputDeviceDetached", it->first.c_str());
                    connectionsToRemove.insert(it->first);
                }
            }
            for (std::set<std::string>::iterator it = connectionsToRemove.begin(); it != connectionsToRemove.end(); ++it) {
                virtualMidi2OutputMap.erase(*it);
            }
        }
        }

        currentConnections.clear();

        // rawmidi 2.0
        if (isMidi2Enabled) {
            card = -1;
            while ((status = snd_card_next(&card)) >= 0 && (card >= 0)) {
                sprintf(name, "hw:%d", card);
                if ((status = snd_ctl_open(&ctl, name, 0)) < 0) {
                    continue;
                }
                snd_card_get_name(card, &deviceName);

                // https://github.com/alsa-project/alsa-lib/blob/master/src/control/control.c
                // https://github.com/alsa-project/alsa-lib/blob/master/include/ump.h
                device = -1;
                for (;;) {
                    status = snd_ctl_ump_next_device(ctl, &device);
                    if (status < 0 || device < 0) {
                        break;
                    }
                    recordUmpOccupiedDevices(ctl, info, card, device, &umpOccupiedHw);
                    snd_rawmidi_info_set_device(info, device);

                    // NOTE: this needs ALSA 1.2.13
                    // snd_ump_endpoint_info_set_device(umpinfo, device);
                    snd_ctl_ump_endpoint_info(ctl, umpinfo);

                    // sub devices: input
                    subs = rawmidiSubdeviceCount(ctl, info, device, SND_RAWMIDI_STREAM_INPUT);
                    for (sub = 0; sub < subs; sub++) {
                        sprintf(sub_name, "hw:%d,%d,%d", card, device, sub);
                        sprintf(deviceId, "hw2i:%d-%d-%d", card, device, sub);
                        currentConnections.insert(deviceId);

                        std::lock_guard<std::mutex> lock(midi2InputMapMutex);
                        if (midi2InputMap.find(deviceId) == midi2InputMap.end()) {
                            snd_ump_t* midiInput = NULL;
                            snd_ump_open(&midiInput, NULL, sub_name, SND_RAWMIDI_NONBLOCK);
                            if (midiInput) {
                                if (deviceNames.find(deviceId) == deviceNames.end()) {
                                    deviceNames.insert(std::make_pair(deviceId, rawmidiSubdeviceName(
                                        ctl, info, device, SND_RAWMIDI_STREAM_INPUT, sub, deviceName)));
                                }
                                midi2InputMap.insert(std::make_pair(deviceId, midiInput));

                                // input watcher thread
                                std::string deviceIdStr = deviceId;
                                std::thread midiInputThread(midi2EventWatcher, deviceIdStr, midiInput);
                                midiInputThread.detach();

                                UnitySendMessage(GAME_OBJECT_NAME, "OnMidi2InputDeviceAttached", deviceId);
                            }
                        }
                    }

                    snd_ctl_ump_endpoint_info(ctl, umpinfo);
                    // sub devices: output
                    subs = rawmidiSubdeviceCount(ctl, info, device, SND_RAWMIDI_STREAM_OUTPUT);
                    for (sub = 0; sub < subs; sub++) {
                        sprintf(sub_name, "hw:%d,%d,%d", card, device, sub);
                        sprintf(deviceId, "hw2o:%d-%d-%d", card, device, sub);

                        currentConnections.insert(deviceId);
                        std::lock_guard<std::mutex> lock(midi2OutputMapMutex); // THIS blocks thread
                        if (midi2OutputMap.find(deviceId) == midi2OutputMap.end()) {
                            snd_ump_t* midiOutput = NULL;
                            int openResult = snd_ump_open(NULL, &midiOutput, sub_name, SND_RAWMIDI_NONBLOCK); // never returns 
                            if (midiOutput) {
                                if (deviceNames.find(deviceId) == deviceNames.end()) {
                                    deviceNames.insert(std::make_pair(deviceId, rawmidiSubdeviceName(
                                        ctl, info, device, SND_RAWMIDI_STREAM_OUTPUT, sub, deviceName)));
                                }
                                midi2OutputMap.insert(std::make_pair(deviceId, midiOutput));

                                UnitySendMessage(GAME_OBJECT_NAME, "OnMidi2OutputDeviceAttached", deviceId);
                            }
                        }
                    }
                }

                snd_ctl_close(ctl);
            }
        }

        // rawmidi 1.0
        if (isMidi1Enabled) {
            card = -1;
            while ((status = snd_card_next(&card)) >= 0 && (card >= 0)) {
                sprintf(name, "hw:%d", card);
                if ((status = snd_ctl_open(&ctl, name, 0)) < 0) {
                    continue;
                }
                snd_card_get_name(card, &deviceName);

                // rawmidi(midi 1.0)
                device = -1;
                for (;;) {
                    status = snd_ctl_rawmidi_next_device(ctl, &device);
                    if (status < 0 || device < 0) {
                        break;
                    }
                    if (shouldSkipMidi1Rawmidi(ctl, info, card, device, isMidi2Enabled, umpOccupiedHw)) {
                        continue;
                    }
                    snd_rawmidi_info_set_device(info, device);

                    // sub devices: input
                    subs = rawmidiSubdeviceCount(ctl, info, device, SND_RAWMIDI_STREAM_INPUT);
                    for (sub = 0; sub < subs; sub++) {
                        sprintf(sub_name, "hw:%d,%d,%d", card, device, sub);
                        sprintf(deviceId, "hwi:%d-%d-%d", card, device, sub);
                        currentConnections.insert(deviceId);

                        std::lock_guard<std::mutex> lock(midiInputMapMutex);
                        if (midiInputMap.find(deviceId) == midiInputMap.end()) {
                            snd_rawmidi_t* midiInput = NULL;
                            snd_rawmidi_open(&midiInput, NULL, sub_name, SND_RAWMIDI_NONBLOCK);
                            if (midiInput) {
                                snd_rawmidi_read(midiInput, NULL, 0);
                                if (deviceNames.find(deviceId) == deviceNames.end()) {
                                    deviceNames.insert(std::make_pair(deviceId, rawmidiSubdeviceName(
                                        ctl, info, device, SND_RAWMIDI_STREAM_INPUT, sub, deviceName)));
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
                    subs = rawmidiSubdeviceCount(ctl, info, device, SND_RAWMIDI_STREAM_OUTPUT);
                    for (sub = 0; sub < subs; sub++) {
                        sprintf(sub_name, "hw:%d,%d,%d", card, device, sub);
                        sprintf(deviceId, "hwo:%d-%d-%d", card, device, sub);
                        currentConnections.insert(deviceId);

                        std::lock_guard<std::mutex> lock(midiOutputMapMutex);
                        if (midiOutputMap.find(deviceId) == midiOutputMap.end()) {
                            snd_rawmidi_t* midiOutput = NULL;
                            snd_rawmidi_open(NULL, &midiOutput, sub_name, SND_RAWMIDI_NONBLOCK);
                            if (midiOutput) {
                                if (deviceNames.find(deviceId) == deviceNames.end()) {
                                    deviceNames.insert(std::make_pair(deviceId, rawmidiSubdeviceName(
                                        ctl, info, device, SND_RAWMIDI_STREAM_OUTPUT, sub, deviceName)));
                                }
                                midiOutputMap.insert(std::make_pair(deviceId, midiOutput));

                                UnitySendMessage(GAME_OBJECT_NAME, "OnMidiOutputDeviceAttached", deviceId);
                            }
                        }
                    }
                }

                // close
                snd_ctl_close(ctl);
            }
        }

        {
            connectionsToRemove.clear();
            {
                std::lock_guard<std::mutex> lock(midiInputMapMutex);
                for (std::map<std::string, snd_rawmidi_t*>::iterator it = midiInputMap.begin(); it != midiInputMap.end(); ++it) {
                    if (currentConnections.find(it->first) == currentConnections.end()) {
                        connectionsToRemove.insert(it->first);
                    }
                }
            }
            for (std::set<std::string>::iterator it = connectionsToRemove.begin(); it != connectionsToRemove.end(); ++it) {
                closeRawmidiAndNotify(midiInputMap, midiInputMapMutex, *it, "OnMidiInputDeviceDetached");
            }
        }
        {
            connectionsToRemove.clear();
            {
                std::lock_guard<std::mutex> lock(midiOutputMapMutex);
                for (std::map<std::string, snd_rawmidi_t*>::iterator it = midiOutputMap.begin(); it != midiOutputMap.end(); ++it) {
                    if (currentConnections.find(it->first) == currentConnections.end()) {
                        connectionsToRemove.insert(it->first);
                    }
                }
            }
            for (std::set<std::string>::iterator it = connectionsToRemove.begin(); it != connectionsToRemove.end(); ++it) {
                closeRawmidiAndNotify(midiOutputMap, midiOutputMapMutex, *it, "OnMidiOutputDeviceDetached");
            }
        }
        {
            connectionsToRemove.clear();
            {
                std::lock_guard<std::mutex> lock(midi2InputMapMutex);
                for (std::map<std::string, snd_ump_t*>::iterator it = midi2InputMap.begin(); it != midi2InputMap.end(); ++it) {
                    if (currentConnections.find(it->first) == currentConnections.end()) {
                        connectionsToRemove.insert(it->first);
                    }
                }
            }
            for (std::set<std::string>::iterator it = connectionsToRemove.begin(); it != connectionsToRemove.end(); ++it) {
                closeUmpAndNotify(midi2InputMap, midi2InputMapMutex, *it, "OnMidi2InputDeviceDetached");
            }
        }
        {
            connectionsToRemove.clear();
            {
                std::lock_guard<std::mutex> lock(midi2OutputMapMutex);
                for (std::map<std::string, snd_ump_t*>::iterator it = midi2OutputMap.begin(); it != midi2OutputMap.end(); ++it) {
                    if (currentConnections.find(it->first) == currentConnections.end()) {
                        connectionsToRemove.insert(it->first);
                    }
                }
            }
            for (std::set<std::string>::iterator it = connectionsToRemove.begin(); it != connectionsToRemove.end(); ++it) {
                closeUmpAndNotify(midi2OutputMap, midi2OutputMapMutex, *it, "OnMidi2OutputDeviceDetached");
            }
        }

        std::this_thread::sleep_for(100ms);
    }

    // terminated, leftover cleanup
    shutdownMidi1Resources();
    shutdownMidi2Resources();
    {
        std::lock_guard<std::mutex> lock(deviceNamesMutex);
        deviceNames.clear();
    }
}

void SetSendMessageCallback(OnSendMessageDelegate callback) {
   onSendMessage = callback;
}

static void startWatcherThreadsIfNeeded() {
    if (isStopped) {
        isStopped = false;
        std::thread midiConnectionThread(midiConnectionWatcher);
        midiConnectionThread.detach();
        std::thread midiInputThread(virtualMidiEventWatcher);
        midiInputThread.detach();
        std::thread midi2InputThread(virtualMidi2EventWatcher);
        midi2InputThread.detach();
    }
}

void InitializeMidiLinux() {
    isMidi1Enabled = true;
    openSeqClient(seqMidi1, "Midi Handler", SND_SEQ_CLIENT_LEGACY_MIDI, SEQ_PORT_CAPS_MIDI1);
    startWatcherThreadsIfNeeded();
}

void InitializeMidi2Linux() {
    isMidi2Enabled = true;
    openSeqClient(seqMidi2, "Midi2 Handler", SND_SEQ_CLIENT_UMP_MIDI_2_0, SEQ_PORT_CAPS_MIDI2);
    startWatcherThreadsIfNeeded();
}

void TerminateMidiLinux() {
    isMidi1Enabled = false;
    shutdownMidi1Resources();
    if (!isMidi2Enabled) {
        isStopped = true;
    }
}

void TerminateMidi2Linux() {
    isMidi2Enabled = false;
    shutdownMidi2Resources();
    if (!isMidi1Enabled) {
        isStopped = true;
    }
}

const char* GetDeviceNameLinux(const char* deviceId) {
    if (deviceNames.find(deviceId) != deviceNames.end()) {
        return strdup(deviceNames[deviceId].c_str());
    }

    return NULL;
}

void SendMidiNoteOff(const char* deviceId, char channel, char note, char velocity) {
    {
        std::lock_guard<std::mutex> lock(midiOutputMapMutex);
        decltype(midiOutputMap)::iterator it = midiOutputMap.find(deviceId);
        if (it != midiOutputMap.end()) {
            char midi[3] = {(char)(0x80 | channel), note, velocity};
            snd_rawmidi_write(it->second, midi, 3);
            snd_rawmidi_drain(it->second);
        }
    }

    {
        std::lock_guard<std::mutex> lock(virtualMidiOutputMapMutex);
        decltype(virtualMidiOutputMap)::iterator it2 = virtualMidiOutputMap.find(deviceId);
        if (it2 != virtualMidiOutputMap.end()) {
            snd_seq_event_t ev;
            initSeqEventDirect(&ev, seqMidi1.portNumber, it2->second);

            snd_seq_ev_set_noteoff(&ev, channel, note, velocity);
            outputSeqEvent(seqMidi1, &ev);
        }
    }
}

void SendMidiNoteOn(const char* deviceId, char channel, char note, char velocity) {
    {
        std::lock_guard<std::mutex> lock(midiOutputMapMutex);
        decltype(midiOutputMap)::iterator it = midiOutputMap.find(deviceId);
        if (it != midiOutputMap.end()) {
            char midi[3] = {(char)(0x90 | channel), note, velocity};
            snd_rawmidi_write(it->second, midi, 3);
            snd_rawmidi_drain(it->second);
        }
    }

    {
        std::lock_guard<std::mutex> lock(virtualMidiOutputMapMutex);
        decltype(virtualMidiOutputMap)::iterator it2 = virtualMidiOutputMap.find(deviceId);
        if (it2 != virtualMidiOutputMap.end()) {
            snd_seq_event_t ev;
            initSeqEventDirect(&ev, seqMidi1.portNumber, it2->second);

            snd_seq_ev_set_noteon(&ev, channel, note, velocity);
            outputSeqEvent(seqMidi1, &ev);
        }
    }
}

void SendMidiPolyphonicAftertouch(const char* deviceId, char channel, char note, char pressure) {
    {
        std::lock_guard<std::mutex> lock(midiOutputMapMutex);
        decltype(midiOutputMap)::iterator it = midiOutputMap.find(deviceId);
        if (it != midiOutputMap.end()) {
            char midi[3] = {(char)(0xa0 | channel), note, pressure};
            snd_rawmidi_write(it->second, midi, 3);
            snd_rawmidi_drain(it->second);
        }
    }

    {
        std::lock_guard<std::mutex> lock(virtualMidiOutputMapMutex);
        decltype(virtualMidiOutputMap)::iterator it2 = virtualMidiOutputMap.find(deviceId);
        if (it2 != virtualMidiOutputMap.end()) {
            snd_seq_event_t ev;
            initSeqEventDirect(&ev, seqMidi1.portNumber, it2->second);

            snd_seq_ev_set_keypress(&ev, channel, note, pressure);
            outputSeqEvent(seqMidi1, &ev);
        }
    }
}

void SendMidiControlChange(const char* deviceId, char channel, char func, char value) {
    {
        std::lock_guard<std::mutex> lock(midiOutputMapMutex);
        decltype(midiOutputMap)::iterator it = midiOutputMap.find(deviceId);
        if (it != midiOutputMap.end()) {
            char midi[3] = {(char)(0xb0 | channel), func, value};
            snd_rawmidi_write(it->second, midi, 3);
            snd_rawmidi_drain(it->second);
        }
    }

    {
        std::lock_guard<std::mutex> lock(virtualMidiOutputMapMutex);
        decltype(virtualMidiOutputMap)::iterator it2 = virtualMidiOutputMap.find(deviceId);
        if (it2 != virtualMidiOutputMap.end()) {
            snd_seq_event_t ev;
            initSeqEventDirect(&ev, seqMidi1.portNumber, it2->second);

            snd_seq_ev_set_controller(&ev, channel, func, value);
            outputSeqEvent(seqMidi1, &ev);
        }
    }
}

void SendMidiProgramChange(const char* deviceId, char channel, char program) {
    {
        std::lock_guard<std::mutex> lock(midiOutputMapMutex);
        decltype(midiOutputMap)::iterator it = midiOutputMap.find(deviceId);
        if (it != midiOutputMap.end()) {
            char midi[2] = {(char)(0xc0 | channel), program};
            snd_rawmidi_write(it->second, midi, 2);
            snd_rawmidi_drain(it->second);
        }
    }

    {
        std::lock_guard<std::mutex> lock(virtualMidiOutputMapMutex);
        decltype(virtualMidiOutputMap)::iterator it2 = virtualMidiOutputMap.find(deviceId);
        if (it2 != virtualMidiOutputMap.end()) {
            snd_seq_event_t ev;
            initSeqEventDirect(&ev, seqMidi1.portNumber, it2->second);

            snd_seq_ev_set_pgmchange(&ev, channel, program);
            outputSeqEvent(seqMidi1, &ev);
        }
    }
}

void SendMidiChannelAftertouch(const char* deviceId, char channel, char pressure) {
    {
        std::lock_guard<std::mutex> lock(midiOutputMapMutex);
        decltype(midiOutputMap)::iterator it = midiOutputMap.find(deviceId);
        if (it != midiOutputMap.end()) {
            char midi[2] = {(char)(0xd0 | channel), pressure};
            snd_rawmidi_write(it->second, midi, 2);
            snd_rawmidi_drain(it->second);
        }
    }

    {
        std::lock_guard<std::mutex> lock(virtualMidiOutputMapMutex);
        decltype(virtualMidiOutputMap)::iterator it2 = virtualMidiOutputMap.find(deviceId);
        if (it2 != virtualMidiOutputMap.end()) {
            snd_seq_event_t ev;
            initSeqEventDirect(&ev, seqMidi1.portNumber, it2->second);

            snd_seq_ev_set_chanpress(&ev, channel, pressure);
            outputSeqEvent(seqMidi1, &ev);
        }
    }
}

void SendMidiPitchWheel(const char* deviceId, char channel, short amount) {
    {
        std::lock_guard<std::mutex> lock(midiOutputMapMutex);
        decltype(midiOutputMap)::iterator it = midiOutputMap.find(deviceId);
        if (it != midiOutputMap.end()) {
            char midi[3] = {(char)(0xe0 | channel), (char)(amount & 0x7f), (char)((amount >> 7) & 0x7f)};
            snd_rawmidi_write(it->second, midi, 3);
            snd_rawmidi_drain(it->second);
        }
    }

    {
        std::lock_guard<std::mutex> lock(virtualMidiOutputMapMutex);
        decltype(virtualMidiOutputMap)::iterator it2 = virtualMidiOutputMap.find(deviceId);
        if (it2 != virtualMidiOutputMap.end()) {
            snd_seq_event_t ev;
            initSeqEventDirect(&ev, seqMidi1.portNumber, it2->second);

            snd_seq_ev_set_pitchbend(&ev, channel, amount - 8192);
            outputSeqEvent(seqMidi1, &ev);
        }
    }
}

void SendMidiSystemExclusive(const char* deviceId, unsigned char* data, int length) {
    {
        std::lock_guard<std::mutex> lock(midiOutputMapMutex);
        decltype(midiOutputMap)::iterator it = midiOutputMap.find(deviceId);
        if (it != midiOutputMap.end()) {
            snd_rawmidi_write(it->second, data, length);
            snd_rawmidi_drain(it->second);
        }
    }

    {
        std::lock_guard<std::mutex> lock(virtualMidiOutputMapMutex);
        decltype(virtualMidiOutputMap)::iterator it2 = virtualMidiOutputMap.find(deviceId);
        if (it2 != virtualMidiOutputMap.end()) {
            snd_seq_event_t ev;
            initSeqEventDirect(&ev, seqMidi1.portNumber, it2->second);

            snd_seq_ev_set_sysex(&ev, length, data);
            outputSeqEvent(seqMidi1, &ev);
        }
    }
}

void SendMidiTimeCodeQuarterFrame(const char* deviceId, char value) {
    std::lock_guard<std::mutex> lock(midiOutputMapMutex);
    decltype(midiOutputMap)::iterator it = midiOutputMap.find(deviceId);
    if (it != midiOutputMap.end()) {
        char midi[2] = {(char)0xf1, value};
        snd_rawmidi_write(it->second, midi, 2);
        snd_rawmidi_drain(it->second);
    }
}

void SendMidiSongPositionPointer(const char* deviceId, short position) {
    std::lock_guard<std::mutex> lock(midiOutputMapMutex);
    decltype(midiOutputMap)::iterator it = midiOutputMap.find(deviceId);
    if (it != midiOutputMap.end()) {
        char midi[3] = {(char)0xf2, (char)(position & 0x7f), (char)((position >> 7) & 0x7f)};
        snd_rawmidi_write(it->second, midi, 3);
        snd_rawmidi_drain(it->second);
    }
}

void SendMidiSongSelect(const char* deviceId, char song) {
    std::lock_guard<std::mutex> lock(midiOutputMapMutex);
    decltype(midiOutputMap)::iterator it = midiOutputMap.find(deviceId);
    if (it != midiOutputMap.end()) {
        char midi[2] = {(char)0xf3, song};
        snd_rawmidi_write(it->second, midi, 2);
        snd_rawmidi_drain(it->second);
    }
}

void SendMidiTuneRequest(const char* deviceId) {
    std::lock_guard<std::mutex> lock(midiOutputMapMutex);
    decltype(midiOutputMap)::iterator it = midiOutputMap.find(deviceId);
    if (it != midiOutputMap.end()) {
        char midi[1] = {(char)0xf6};
        snd_rawmidi_write(it->second, midi, 1);
        snd_rawmidi_drain(it->second);
    }
}

void SendMidiTimingClock(const char* deviceId) {
    std::lock_guard<std::mutex> lock(midiOutputMapMutex);
    decltype(midiOutputMap)::iterator it = midiOutputMap.find(deviceId);
    if (it != midiOutputMap.end()) {
        char midi[1] = {(char)0xf8};
        snd_rawmidi_write(it->second, midi, 1);
        snd_rawmidi_drain(it->second);
    }
}

void SendMidiStart(const char* deviceId) {
    {
        std::lock_guard<std::mutex> lock(midiOutputMapMutex);
        decltype(midiOutputMap)::iterator it = midiOutputMap.find(deviceId);
        if (it != midiOutputMap.end()) {
            char midi[1] = {(char)0xfa};
            snd_rawmidi_write(it->second, midi, 1);
            snd_rawmidi_drain(it->second);
        }
    }

    {
        std::lock_guard<std::mutex> lock(virtualMidiOutputMapMutex);
        decltype(virtualMidiOutputMap)::iterator it2 = virtualMidiOutputMap.find(deviceId);
        if (it2 != virtualMidiOutputMap.end()) {
            sendSeqRealtimeEvent(it2->second, SND_SEQ_EVENT_START);
        }
    }
}

void SendMidiContinue(const char* deviceId) {
    {
        std::lock_guard<std::mutex> lock(midiOutputMapMutex);
        decltype(midiOutputMap)::iterator it = midiOutputMap.find(deviceId);
        if (it != midiOutputMap.end()) {
            char midi[1] = {(char)0xfb};
            snd_rawmidi_write(it->second, midi, 1);
            snd_rawmidi_drain(it->second);
        }
    }

    {
        std::lock_guard<std::mutex> lock(virtualMidiOutputMapMutex);
        decltype(virtualMidiOutputMap)::iterator it2 = virtualMidiOutputMap.find(deviceId);
        if (it2 != virtualMidiOutputMap.end()) {
            sendSeqRealtimeEvent(it2->second, SND_SEQ_EVENT_CONTINUE);
        }
    }
}

void SendMidiStop(const char* deviceId) {
    {
        std::lock_guard<std::mutex> lock(midiOutputMapMutex);
        decltype(midiOutputMap)::iterator it = midiOutputMap.find(deviceId);
        if (it != midiOutputMap.end()) {
            char midi[1] = {(char)0xfc};
            snd_rawmidi_write(it->second, midi, 1);
            snd_rawmidi_drain(it->second);
        }
    }

    {
        std::lock_guard<std::mutex> lock(virtualMidiOutputMapMutex);
        decltype(virtualMidiOutputMap)::iterator it2 = virtualMidiOutputMap.find(deviceId);
        if (it2 != virtualMidiOutputMap.end()) {
            sendSeqRealtimeEvent(it2->second, SND_SEQ_EVENT_STOP);
        }
    }
}

void SendMidiActiveSensing(const char* deviceId) {
    std::lock_guard<std::mutex> lock(midiOutputMapMutex);
    decltype(midiOutputMap)::iterator it = midiOutputMap.find(deviceId);
    if (it != midiOutputMap.end()) {
        char midi[1] = {(char)0xfe};
        snd_rawmidi_write(it->second, midi, 1);
        snd_rawmidi_drain(it->second);
    }
}

void SendMidiReset(const char* deviceId) {
    std::lock_guard<std::mutex> lock(midiOutputMapMutex);
    decltype(midiOutputMap)::iterator it = midiOutputMap.find(deviceId);
    if (it != midiOutputMap.end()) {
        char midi[1] = {(char)0xff};
        snd_rawmidi_write(it->second, midi, 1);
        snd_rawmidi_drain(it->second);
    }
}

void SendUmpMessage(const char* deviceId, uint32_t* ump, int length) {
    if (ump == nullptr || length <= 0) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(midi2OutputMapMutex);
        decltype(midi2OutputMap)::iterator it = midi2OutputMap.find(deviceId);
        if (it != midi2OutputMap.end()) {
            snd_ump_write(it->second, ump, static_cast<size_t>(length) * sizeof(uint32_t));
        }
    }

    {
        std::lock_guard<std::mutex> lock(virtualMidi2OutputMapMutex);
        decltype(virtualMidi2OutputMap)::iterator it2 = virtualMidi2OutputMap.find(deviceId);
        if (it2 != virtualMidi2OutputMap.end()) {
            int offset = 0;
            while (offset < length) {
                int packetWords = umpPacketWordCount(ump[offset]);
                if (offset + packetWords > length) {
                    packetWords = length - offset;
                }
                if (packetWords > 4) {
                    packetWords = 4;
                }

                snd_seq_ump_event_t ev;
                initSeqUmpEventDirect(&ev, seqMidi2.portNumber, it2->second);
                snd_seq_ev_set_ump(&ev);
                // snd_seq_ev_set_ump_data takes a byte count, max 16
                snd_seq_ev_set_ump_data(&ev, ump + offset, static_cast<size_t>(packetWords) * sizeof(uint32_t));
                outputSeqUmpEvent(seqMidi2, &ev);
                offset += packetWords;
            }
        }
    }
}

/*
 * Midi.cpp
 *
 * Created on: 15 Jan 2016
 * Author: giulio
 */

#include "Midi.h"
#include <fcntl.h>
#include <errno.h>
#include <glob.h>


#define kMidiInput 0
#define kMidiOutput 1

//static void  print_midi_ports          (void);
//static void  print_card_list           (void);
//static void  list_midi_devices_on_card (int card);
//static void  list_subdevice_info       (snd_ctl_t *ctl, int card, int device);
static int   is_input                  (snd_ctl_t *ctl, int card, int device, int sub);
static int   is_output                 (snd_ctl_t *ctl, int card, int device, int sub);
static void  error                     (const char *format, ...);

///////////////////////////////////////////////////////////////////////////

midi_byte_t midiMessageStatusBytes[midiMessageStatusBytesLength]=
{
	0x80, /* note off */
	0x90, /* note on */
	0xA0, /* polyphonic key pressure */
	0xB0, /* control change */
	0xC0, /* program change */
	0xD0, /* channel key pressure */
	0xE0, /* pitch bend change */
	0
};

unsigned int midiMessageNumDataBytes[midiMessageStatusBytesLength]={2, 2, 2, 2, 1, 1, 2, 0};

bool Midi::staticConstructed;
AuxiliaryTask Midi::midiInputTask;
AuxiliaryTask Midi::midiOutputTask;
std::vector<Midi *> Midi::objAddrs[2];

int MidiParser::parse(midi_byte_t* input, unsigned int length){
	unsigned int consumedBytes = 0;
	for(unsigned int n = 0; n < length; n++){
		consumedBytes++;
		if(waitingForStatus == true){
			int statusByte = input[n];
			MidiMessageType newType = kmmNone;
			if (statusByte >= 0x80 && statusByte < 0xF0){//it actually is a status byte
				for(int n = 0; n < midiMessageStatusBytesLength; n++){ //find the statusByte in the array
					if(midiMessageStatusBytes[n] == (statusByte&0xf0)){
						newType = (MidiMessageType)n;
						break;
					}
				}
				elapsedDataBytes = 0;
				waitingForStatus = false;
				messages[writePointer].setType(newType);
				messages[writePointer].setChannel((midi_byte_t)(statusByte&0xf));
				consumedBytes++;
			} else if (statusByte == 0xF0) {
				//sysex!!!
				waitingForStatus = false;
				receivingSysex = true;
				rt_printf("Receiving sysex\n");
			} else { // either something went wrong or it's a system message
				continue;
			}
		} else if (receivingSysex){
			// Just wait for the message to end
			rt_printf("%c", input[n]);
			if(input[n] == 0xF7){
				receivingSysex = false;
				waitingForStatus = true;
				rt_printf("\nCompleted receiving sysex\n");
			}
		} else {
			messages[writePointer].setDataByte(elapsedDataBytes, input[n]);
			elapsedDataBytes++;
			if(elapsedDataBytes == messages[writePointer].getNumDataBytes()){
				// done with the current message
				// call the callback if available
				if(isCallbackEnabled() == true){
					messageReadyCallback(getNextChannelMessage(), callbackArg);
				}
				waitingForStatus = true;
				writePointer++;
				if(writePointer == messages.size()){
					writePointer = 0;
				}
			}
		}
	}

	return consumedBytes;
};


Midi::Midi() : 
alsaIn(NULL), alsaOut(NULL), useAlsaApi(false) {
	outputPort = -1;
	inputPort = -1;
	inputParser = 0;
	size_t inputBytesInitialSize = 1000;
	inputBytes.resize(inputBytesInitialSize);
	outputBytes.resize(inputBytesInitialSize);
	inputBytesWritePointer = 0;
	inputBytesReadPointer = inputBytes.size() - 1;
	if(!staticConstructed){
		staticConstructor();
	}
}

void Midi::staticConstructor(){
	staticConstructed = true;
	midiInputTask = Bela_createAuxiliaryTask(Midi::midiInputLoop, 50, "MidiInput");
	midiOutputTask = Bela_createAuxiliaryTask(Midi::midiOutputLoop, 50, "MidiOutput");
}

Midi::~Midi() {
	if(useAlsaApi) {
		if(alsaOut){
			snd_rawmidi_drain(alsaOut);
			snd_rawmidi_close(alsaOut);
		}
		if(alsaIn){
			snd_rawmidi_drain(alsaIn);
			snd_rawmidi_close(alsaIn);
		}
	}
}

void Midi::enableParser(bool enable){
	if(enable == true){
		delete inputParser;
		inputParser = new MidiParser();
		parserEnabled = true;
	} else {
		delete inputParser;
		parserEnabled = false;
	}
}

void Midi::midiInputLoop(){
	while(!gShouldStop){
		for(unsigned int n = 0; n < objAddrs[kMidiInput].size(); n++){
			objAddrs[kMidiInput][n] -> readInputLoop();
			//printf("read %d\n", n);
			usleep(1000);
		}
	}
	for(unsigned int n = 0; n < objAddrs[kMidiInput].size(); n++){
		if(objAddrs[kMidiInput][n]->useAlsaApi && objAddrs[kMidiInput][n]->alsaIn) {
			snd_rawmidi_drain(objAddrs[kMidiInput][n]->alsaIn);
			snd_rawmidi_close(objAddrs[kMidiInput][n]->alsaIn);
			objAddrs[kMidiInput][n]->alsaIn = NULL;
		}
	}
}

void Midi::midiOutputLoop(){
	while(!gShouldStop){
		for(unsigned int n = 0; n < objAddrs[kMidiOutput].size(); n++){
			objAddrs[kMidiOutput][n] -> writeOutputLoop();
			usleep(1000);
		}
	}
	for(unsigned int n = 0; n < objAddrs[kMidiOutput].size(); n++){
		if(objAddrs[kMidiInput][n]->useAlsaApi && objAddrs[kMidiInput][n]->alsaOut) {
			snd_rawmidi_drain(objAddrs[kMidiOutput][n]->alsaOut);
			snd_rawmidi_close(objAddrs[kMidiOutput][n]->alsaOut);
			objAddrs[kMidiOutput][n]->alsaOut = NULL;
		}
	}
}

void Midi::readInputLoop(){

	while(!gShouldStop){ 
	// this keeps going until the buffer is emptied (probably no more than
	// once or twice)
		int maxBytesToRead = inputBytes.size() - inputBytesWritePointer;
		int ret = maxBytesToRead;
		if(useAlsaApi && alsaIn) {
			ret = snd_rawmidi_read(alsaIn,&inputBytes[inputBytesWritePointer],sizeof(midi_byte_t)*maxBytesToRead);
		} else {
			ret = read(inputPort, &inputBytes[inputBytesWritePointer], sizeof(midi_byte_t)*maxBytesToRead);
		}
		if(ret < 0){
			if(errno != EAGAIN){ // read() would return EAGAIN when no data are available to read just now
				rt_printf("Error while reading midi %d\n", errno);
			}
			return;
		}
		inputBytesWritePointer += ret;
		if(inputBytesWritePointer == inputBytes.size()){ //wrap pointer around
			inputBytesWritePointer = 0;
		}

		if(parserEnabled == true && ret > 0){ // if the parser is enabled and there is new data, send the data to it
			int input;
			while((input=_getInput()) >= 0){
				midi_byte_t inputByte = (midi_byte_t)(input);
				inputParser->parse(&inputByte, 1);
			}
		}
		if(ret < maxBytesToRead){
		// no more data to retrieve at the moment
			return;
		}
		// otherwise there might be more data ready to be read (e.g. if we
		// reached the end of the buffer), so keep going
	} 

}

void Midi::writeOutputLoop(){
	while(!gShouldStop){
	// this keeps going until the buffer is emptied (probably no more than
	// once or twice)
		int length = outputBytesWritePointer - outputBytesReadPointer;
		if(length < 0){
			length = outputBytes.size() - outputBytesReadPointer;
		}
		if(length == 0){ //nothing to be written
			break;
		}
		int ret;
		if(useAlsaApi && alsaOut) {
			ret = snd_rawmidi_write(alsaOut,&outputBytes[outputBytesReadPointer], sizeof(midi_byte_t)*length);
			snd_rawmidi_drain(alsaOut);
		} else {
			ret = write(outputPort, &outputBytes[outputBytesReadPointer], sizeof(midi_byte_t)*length);
		}
		outputBytesReadPointer += ret;
		if(outputBytesReadPointer >= outputBytes.size()){
			outputBytesReadPointer -= outputBytes.size();
		} else {
			break;
		}
		if(ret < 0){ //error occurred
			rt_printf("Error occurred while writing: %d\n", strerror(errno));
			usleep(10000);
		}
	}
}

void Midi::useAlsa(bool f) {
	useAlsaApi = f;
}

int Midi::readFrom(const char* port){
	objAddrs[kMidiInput].push_back(this);
	if(useAlsaApi) {
		int err = snd_rawmidi_open(&alsaIn,NULL,port,SND_RAWMIDI_NONBLOCK);
		if (err) {
			rt_printf("readFrom snd_rawmidi_open %s failed: %d\n",port,err);
			return -1;
		}
		rt_printf("Reading from Alsa midi device %s\n", port);
	} else {
		inputPort = open(port, O_RDONLY | O_NONBLOCK | O_NOCTTY);
		if(inputPort < 0){
			return -1;
		}
		rt_printf("Reading from Midi port %s\n", port);
	}
	Bela_scheduleAuxiliaryTask(midiInputTask);
	return 1;
}

int Midi::writeTo(const char* port){
	objAddrs[kMidiOutput].push_back(this);
	if(useAlsaApi) {
		int err = snd_rawmidi_open(NULL, &alsaOut, port,0);
		if (err) {
			rt_printf("writeTo snd_rawmidi_open %s failed: %d\n",port,err);
			return -1;
		}
		rt_printf("Writing to Alsa midi device %s\n", port);
	} else {
		outputPort = open(port, O_WRONLY, 0);
		if(outputPort < 0){
			return -1;
		}
		rt_printf("Writing to Midi port %s\n", port);
	}
	Bela_scheduleAuxiliaryTask(midiOutputTask);
	return 1;
}

void Midi::createAllPorts(std::vector<Midi*>& ports, bool useAlsaApi, bool useParser){
	if(useAlsaApi){
		int card = -1;
		int status;
		while((status = snd_card_next(&card)) == 0){
			if(card < 0){
				break;
			}
			snd_ctl_t *ctl;
			char name[32];
			int device = -1;
			int status;
   			sprintf(name, "hw:%d", card);
			if ((status = snd_ctl_open(&ctl, name, 0)) < 0) {
				error("cannot open control for card %d: %s", card, snd_strerror(status));
				return;
			}
			do {
				status = snd_ctl_rawmidi_next_device(ctl, &device);
				if (status < 0) {
					error("cannot determine device number: %s", snd_strerror(status));
					break;
				}
				if (device >= 0) {
					printf("device: %d\n", device);
					snd_rawmidi_info_t *info;
					snd_rawmidi_info_alloca(&info);
					snd_rawmidi_info_set_device(info, device);
					
					// count subdevices:
					snd_rawmidi_info_set_stream(info, SND_RAWMIDI_STREAM_INPUT);
					snd_ctl_rawmidi_info(ctl, info);
					unsigned int subs_in = snd_rawmidi_info_get_subdevices_count(info);
					snd_rawmidi_info_set_stream(info, SND_RAWMIDI_STREAM_OUTPUT);
					snd_ctl_rawmidi_info(ctl, info);
					unsigned int subs_out = snd_rawmidi_info_get_subdevices_count(info);
					//number of subdevices is max (inputs, outputs);
					unsigned int subs = subs_in > subs_out ? subs_in : subs_out;

					for(unsigned int sub = 0; sub < subs; ++sub){
						bool in = false;
						bool out = false;
						if ((status = is_output(ctl, card, device, sub)) < 0) {
							error("cannot get rawmidi information %d:%d: %s",
							card, device, snd_strerror(status));
							return;
						} else if (status){
							out = true;
							// writeTo
						}

						if (status == 0) {
							if ((status = is_input(ctl, card, device, sub)) < 0) {
								error("cannot get rawmidi information %d:%d: %s",
								card, device, snd_strerror(status));
								return;
							}
						} else if (status) {
							in = true;
							// readfrom
						}

						if(in || out){
							ports.resize(ports.size() + 1);
							unsigned int index = ports.size() - 1;
							ports[index] = new Midi();
							ports[index]->useAlsa(true);
							sprintf(name, "hw:%d,%d,%d", card, device, sub);
							if(in){
								printf("Reading from: %s\n", name);
								ports[index]->readFrom(name);
							}
							if(out){
								printf("Writing to: %s\n", name);
								ports[index]->writeTo(name);
							}
						}
					}
				}
			} while (device >= 0);
			snd_ctl_close(ctl);
		}
	} else {
		unsigned int numPorts = 0;
		char** paths = NULL;
		glob_t pglob;
		glob("/dev/midi*", 0, NULL, &pglob);
		numPorts = pglob.gl_pathc;
		paths = pglob.gl_pathv;
		ports.resize(numPorts);
		rt_printf("Number of MIDI devices available: %d\n", numPorts);
		for(unsigned int n = 0; n < ports.size(); ++n){
			rt_printf("%s\n", paths[n]);	
			ports[n] = new Midi();
			ports[n]->writeTo(paths[n]);
			ports[n]->readFrom(paths[n]);
		}
	}
}

void Midi::destroyPorts(std::vector<Midi*>& ports){
	for(unsigned int n = 0; n < ports.size(); ++n){
		delete ports[n];
	}
}

int Midi::_getInput(){
	if( (useAlsaApi && !alsaIn ) || (!useAlsaApi && inputPort < 0) )
		return -2;
	if(inputBytesReadPointer == inputBytesWritePointer){
		return -1; // no bytes to read
	}
	midi_byte_t inputMessage = inputBytes[inputBytesReadPointer++];
	if(inputBytesReadPointer == inputBytes.size()){ // wrap pointer
		inputBytesReadPointer = 0;
	}
	return inputMessage;
}

int Midi::getInput(){
	if(parserEnabled == true) {
		return -3;
	}
	return _getInput();
}

MidiParser* Midi::getParser(){
	if(parserEnabled == false){
		return 0;
	}
	return inputParser;
}

void Midi::writeOutput(midi_byte_t byte){
	outputBytes[outputBytesWritePointer++] = byte;
	if(outputBytesWritePointer >= outputBytes.size()){
		outputBytesWritePointer = 0;
	}
}

void Midi::writeOutput(midi_byte_t* bytes, unsigned int length){
	for(unsigned int n = 0; n < length; ++n){
		writeOutput(bytes[n]);
	}
}

midi_byte_t Midi::makeStatusByte(midi_byte_t statusCode, midi_byte_t channel){
	return (statusCode & 0xF0) | (channel & 0x0F);
}

void Midi::writeMessage(midi_byte_t statusCode, midi_byte_t channel, midi_byte_t dataByte){
	midi_byte_t bytes[2] = {makeStatusByte(statusCode, channel), dataByte & 0x7F};
	writeOutput(bytes, 2);
}

void Midi::writeMessage(midi_byte_t statusCode, midi_byte_t channel, midi_byte_t dataByte1, midi_byte_t dataByte2){
	midi_byte_t bytes[3] = {makeStatusByte(statusCode, channel), dataByte1 & 0x7F, dataByte2 & 0x7F};
	writeOutput(bytes, 3);
}

void Midi::writeNoteOff(midi_byte_t channel, midi_byte_t pitch, midi_byte_t velocity){
	writeMessage(midiMessageStatusBytes[kmmNoteOff], channel, pitch, velocity);
}

void Midi::writeNoteOn(midi_byte_t channel, midi_byte_t pitch, midi_byte_t velocity){
	writeMessage(midiMessageStatusBytes[kmmNoteOn], channel, pitch, velocity);
}

void Midi::writePolyphonicKeyPressure(midi_byte_t channel, midi_byte_t pitch, midi_byte_t pressure){
	writeMessage(midiMessageStatusBytes[kmmPolyphonicKeyPressure], channel, pitch, pressure);
}

void Midi::writeControlChange(midi_byte_t channel, midi_byte_t controller, midi_byte_t value){
	writeMessage(midiMessageStatusBytes[kmmControlChange], channel, controller, value);
}

void Midi::writeProgramChange(midi_byte_t channel, midi_byte_t program){
	writeMessage(midiMessageStatusBytes[kmmProgramChange], channel, program);
}

void Midi::writeChannelPressure(midi_byte_t channel, midi_byte_t pressure){
	writeMessage(midiMessageStatusBytes[kmmChannelPressure], channel, pressure);
}

void Midi::writePitchBend(midi_byte_t channel, uint16_t bend){
	// the first ``bend'' is clamped with & 0x7F in writeMessage 
	writeMessage(midiMessageStatusBytes[kmmPitchBend], channel, bend, bend >> 7);
}

MidiChannelMessage::MidiChannelMessage(){};
MidiChannelMessage::MidiChannelMessage(MidiMessageType type){
	setType(type);
};
MidiChannelMessage::~MidiChannelMessage(){};
MidiMessageType MidiChannelMessage::getType(){
	return _type;
};
int MidiChannelMessage::getChannel(){
	return _channel;
};
//int MidiChannelMessage::set(midi_byte_t* input);
//
//int MidiControlChangeMessage ::getValue();
//int MidiControlChangeMessage::set(midi_byte_t* input){
//	channel = input[0];
//	number = input[1];
//	value = input[2];
//	return 3;
//}

//int MidiNoteMessage::getNote();
//int MidiNoteMessage::getVelocity();

//midi_byte_t MidiProgramChangeMessage::getProgram();
//


// 
// following code borrowed rom:    Craig Stuart Sapp <craig@ccrma.stanford.edu>
// https://ccrma.stanford.edu/~craig/articles/linuxmidi/alsa-1.0/alsarawportlist.c
//

//////////////////////////////
//
// print_card_list -- go through the list of available "soundcards"
//   in the ALSA system, printing their associated numbers and names.
//   Cards may or may not have any MIDI ports available on them (for 
//   example, a card might only have an audio interface).
//
/*
static void print_card_list(void) {
   int status;
   int card = -1;  // use -1 to prime the pump of iterating through card list
   char* longname  = NULL;
   char* shortname = NULL;

   if ((status = snd_card_next(&card)) < 0) {
      error("cannot determine card number: %s", snd_strerror(status));
      return;
   }
   if (card < 0) {
      error("no sound cards found");
      return;
   }
   while (card >= 0) {
      printf("Card %d:", card);
      if ((status = snd_card_get_name(card, &shortname)) < 0) {
         error("cannot determine card shortname: %s", snd_strerror(status));
         break;
      }
      if ((status = snd_card_get_longname(card, &longname)) < 0) {
         error("cannot determine card longname: %s", snd_strerror(status));
         break;
      }
      printf("\tLONG NAME:  %s\n", longname);
      printf("\tSHORT NAME: %s\n", shortname);
      if ((status = snd_card_next(&card)) < 0) {
         error("cannot determine card number: %s", snd_strerror(status));
         break;
      }
   } 
}



//////////////////////////////
//
// print_midi_ports -- go through the list of available "soundcards",
//   checking them to see if there are devices/subdevices on them which
//   can read/write MIDI data.
//

static void print_midi_ports(void) {
   int status;
   int card = -1;  // use -1 to prime the pump of iterating through card list

   if ((status = snd_card_next(&card)) < 0) {
      error("cannot determine card number: %s", snd_strerror(status));
      return;
   }
   if (card < 0) {
      error("no sound cards found");
      return;
   }
   printf("\nDir Device    Name\n");
   printf("====================================\n");
   while (card >= 0) {
      list_midi_devices_on_card(card);
      if ((status = snd_card_next(&card)) < 0) {
         error("cannot determine card number: %s", snd_strerror(status));
         break;
      }
   } 
   printf("\n");
}



//////////////////////////////
//
// list_midi_devices_on_card -- For a particular "card" look at all
//   of the "devices/subdevices" on it and print information about it
//   if it can handle MIDI input and/or output.
//

static void list_midi_devices_on_card(int card) {
   snd_ctl_t *ctl;
   char name[32];
   int device = -1;
   int status;
   printf("list_midi_devices_on_card\n");
   sprintf(name, "hw:%d", card);
   if ((status = snd_ctl_open(&ctl, name, 0)) < 0) {
      error("cannot open control for card %d: %s", card, snd_strerror(status));
      return;
   }
   do {
      status = snd_ctl_rawmidi_next_device(ctl, &device);
      if (status < 0) {
         error("cannot determine device number: %s", snd_strerror(status));
         break;
      }
      if (device >= 0) {
         printf("device: %d\n", device);
         list_subdevice_info(ctl, card, device);
      }
   } while (device >= 0);
   snd_ctl_close(ctl);
}



//////////////////////////////
//
// list_subdevice_info -- Print information about a subdevice
//   of a device of a card if it can handle MIDI input and/or output.
//

static void list_subdevice_info(snd_ctl_t *ctl, int card, int device) {
   snd_rawmidi_info_t *info;
   const char *name;
   const char *sub_name;
   int subs, subs_in, subs_out;
   int sub, in, out;
   int status;

   snd_rawmidi_info_alloca(&info);
   snd_rawmidi_info_set_device(info, device);

   snd_rawmidi_info_set_stream(info, SND_RAWMIDI_STREAM_INPUT);
   snd_ctl_rawmidi_info(ctl, info);
   subs_in = snd_rawmidi_info_get_subdevices_count(info);
   snd_rawmidi_info_set_stream(info, SND_RAWMIDI_STREAM_OUTPUT);
   snd_ctl_rawmidi_info(ctl, info);
   subs_out = snd_rawmidi_info_get_subdevices_count(info);
   subs = subs_in > subs_out ? subs_in : subs_out;

   sub = 0;
   in = out = 0;
   if ((status = is_output(ctl, card, device, sub)) < 0) {
      error("cannot get rawmidi information %d:%d: %s",
            card, device, snd_strerror(status));
      return;
   } else if (status)
      out = 1;

   if (status == 0) {
      if ((status = is_input(ctl, card, device, sub)) < 0) {
         error("cannot get rawmidi information %d:%d: %s",
               card, device, snd_strerror(status));
         return;
      }
   } else if (status) 
      in = 1;

   if (status == 0)
      return;

   name = snd_rawmidi_info_get_name(info);
   sub_name = snd_rawmidi_info_get_subdevice_name(info);
   if (sub_name[0] == '\0') {
      if (subs == 1) {
         printf("%c%c  hw:%d,%d    %s\n", 
                in  ? 'I' : ' ', 
                out ? 'O' : ' ',
                card, device, name);
      } else
         printf("%c%c  hw:%d,%d    %s (%d subdevices)\n",
                in  ? 'I' : ' ', 
                out ? 'O' : ' ',
                card, device, name, subs);
   } else {
      sub = 0;
      for (;;) {
         printf("%c%c  hw:%d,%d,%d  %s\n",
                in ? 'I' : ' ', out ? 'O' : ' ',
                card, device, sub, sub_name);
         if (++sub >= subs)
            break;

         in = is_input(ctl, card, device, sub);
         out = is_output(ctl, card, device, sub);
         snd_rawmidi_info_set_subdevice(info, sub);
         if (out) {
            snd_rawmidi_info_set_stream(info, SND_RAWMIDI_STREAM_OUTPUT);
            if ((status = snd_ctl_rawmidi_info(ctl, info)) < 0) {
               error("cannot get rawmidi information %d:%d:%d: %s",
                     card, device, sub, snd_strerror(status));
               break;
            } 
         } else {
            snd_rawmidi_info_set_stream(info, SND_RAWMIDI_STREAM_INPUT);
            if ((status = snd_ctl_rawmidi_info(ctl, info)) < 0) {
               error("cannot get rawmidi information %d:%d:%d: %s",
                     card, device, sub, snd_strerror(status));
               break;
            }
         }
         sub_name = snd_rawmidi_info_get_subdevice_name(info);
      }
   }
}

*/

//////////////////////////////
//
// is_input -- returns true if specified card/device/sub can output MIDI data.
//

static int is_input(snd_ctl_t *ctl, int card, int device, int sub) {
   snd_rawmidi_info_t *info;
   int status;

   snd_rawmidi_info_alloca(&info);
   snd_rawmidi_info_set_device(info, device);
   snd_rawmidi_info_set_subdevice(info, sub);
   snd_rawmidi_info_set_stream(info, SND_RAWMIDI_STREAM_INPUT);
   
   if ((status = snd_ctl_rawmidi_info(ctl, info)) < 0 && status != -ENXIO) {
      return status;
   } else if (status == 0) {
      return 1;
   }

   return 0;
}



//////////////////////////////
//
// is_output -- returns true if specified card/device/sub can output MIDI data.
//

static int is_output(snd_ctl_t *ctl, int card, int device, int sub) {
   snd_rawmidi_info_t *info;
   int status;

   snd_rawmidi_info_alloca(&info);
   snd_rawmidi_info_set_device(info, device);
   snd_rawmidi_info_set_subdevice(info, sub);
   snd_rawmidi_info_set_stream(info, SND_RAWMIDI_STREAM_OUTPUT);
   
   if ((status = snd_ctl_rawmidi_info(ctl, info)) < 0 && status != -ENXIO) {
      return status;
   } else if (status == 0) {
      return 1;
   }

   return 0;
}



//////////////////////////////
//
// error -- print error message
//

static void error(const char *format, ...) {
   va_list ap;
   va_start(ap, format);
   vfprintf(stderr, format, ap);
   va_end(ap);
   putc('\n', stderr);
}



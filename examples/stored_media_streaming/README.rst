Files:

/* Common files used by application */
avbtp.c: Common code related to 61883 sits here used by both talker and listener.
avbtp.h: Header file exposing the common api's for talker and listener.


Application: Streaming the stored media

Files: --
Talker side:

 transmit_video.c: Reads the Audio/Video data from Gstreamer pipeline ,convert them to AVB packet and sends to a multicast address
 demux_audio_video.c: constructs the gstreamer pipeline to demux the MP4 file data into audio and video.	


Listner side:

 recieve_video.c: Binds to a multicast address, reads the AVB packets,depacketize it and writes the data to Gstreamer pipeline
 gst_pipeline.c: Gstreamer pipeline to decode and play the audio and video data


Deamons Required: 
MRPD.
GPTP
<Note: GPTP is optional. it works even without GPTP>


Prequisites:- 
1: Install the intel avb driver on both talker and listener side.
2: Install Gstreamer1.0 components.
3. Run the MRPD in both talker and listeners using the command  sudo ./mrpd -mvs - i <if name>.
4. Run the GPTP using the command  ./daemon_cl <if name>


steps to RUN the application for multilistener using the AVB switch: 
<Note: Sycncronisation message between listeners and talker is apllied to one listner only due to constraints in the talker side > 

. Connect talker and listeners to AVB switch.
. Run the MRPD in all the talker and listener side.
. Run the GPTP in all the talker and listener side. 
. RUN the below command at only one listener side. 
   sudo ./recieve_video <if name> 1 AUDIO_VIDEO  
. RUN the below command at all other listeners side.
   for audio and video : 
   sudo ./recieve_video <if name> 0 AUDIO_VIDEO
   for only audio:
   sudo ./recieve_video <if name> 0 AUDIO
   for only video:
   sudo ./recieve_video <if name> 0 VIDEO

.RUN the below command in talker side.
    sudo ./transmit_video <if name> <filename>    
 
Note:all listener side application to be execueted before the talker  

Makefile: For building the application.

Note: Hit Ctrl+c to end the application.

Note: Application supports only MP4 files.

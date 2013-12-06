CFLAGS=$(OPT) -Wall -W -Wno-parentheses
EXTRA_FLAGS=-I ../../lib/igb/ -I../../daemons/mrpd
EXTRA_FLAGS+=-L ../../lib/igb/ -ligb -lpci -lrt -lpthread -lm  -lgstapp-1.0

all:transmit_video  recieve_video
	

transmit_video:transmit_video.c
	gcc -o transmit_video demux_audio_video.c avbtp.c transmit_video.c  `pkg-config --cflags --libs gstreamer-1.0 gstreamer-app-1.0` $(CFLAGS) $(EXTRA_FLAGS)

recieve_video:recieve_video.c
	gcc  -Wall gst_plugins.c avbtp.c recieve_video.c -o recieve_video `pkg-config --cflags --libs gstreamer-1.0 gstreamer-app-1.0`  $(CFLAGS) $(EXTRA_FLAGS)
	

clean:
	rm -rf *.o *~
	rm -rf transmit_video
	rm -rf recieve_video
	

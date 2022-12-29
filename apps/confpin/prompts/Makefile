SRCS=$(wildcard *.txt)
WAVS=$(SRCS:.txt=.wav)
DBS=$(SRCS:.txt=.dbs)

all: $(WAVS) 

db_upload: $(DBS)

%.wav : %.txt Makefile
	flite -f $< -o $(basename $@)_16k.wav
##	text2wave $< -o $(basename $@)_16k.wav
	sox $(basename $@)_16k.wav -r 8000 $@
	rm $(basename $@)_16k.wav

%.dbs:  %.wav Makefile
	mkdir -p ../cache/1
	cp $(basename $@).wav ../cache/1
	../tools/push_file $(basename $@) 1
	touch $(basename $@).dbs
	
clean:
	rm -f *.dbs *.wav
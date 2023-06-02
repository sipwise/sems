
all: participant.py conftable.py callbox.py account.py

install: all
	cp *.py tablet/conf

%.py: %.ui
	pyuic4 $< -o $@

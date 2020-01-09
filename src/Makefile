CC=gcc -Wall -fno-exceptions
COPTS=-Os

TARGET=cpuspeed
DEBUG_TARGET=cpuspeed-debug

all: $(TARGET)
debug: $(DEBUG_TARGET)
fake: $(FAKE_TARGET)

$(TARGET): cpuspeed.cc
	$(CC) -c $(COPTS) cpuspeed.cc
	$(CC) cpuspeed.o -o $(TARGET)
	strip $(TARGET)

# Debug target
$(DEBUG_TARGET): cpuspeed.cc
	$(CC) -c -g -DDEBUG -o cpuspeed_debug.o cpuspeed.cc
	$(CC) cpuspeed_debug.o -o $(DEBUG_TARGET)

install: install_redhat

install_redhat: $(TARGET)
	cp -f $(TARGET) /sbin
	cp -f cpuspeed.rc.redhat /etc/rc.d/init.d/cpuspeed
	test -f /etc/sysconfig/cpuspeed || cp cpuspeed.conf /etc/sysconfig/cpuspeed
	chkconfig cpuspeed reset
	service cpuspeed restart

install_suse: $(TARGET)
	cp -f $(TARGET) /sbin
	cp -f cpuspeed.rc.suse /etc/init.d/boot.cpuspeed
	test -f /etc/sysconfig/powersave/cpuspeed || cp cpuspeed.conf /etc/sysconfig/powersave/cpuspeed
	chkconfig boot.cpuspeed on
	/etc/init.d/boot.cpuspeed restart

install_debian: $(TARGET)
	cp -f $(TARGET) /sbin
	cp -f cpuspeed.rc.debian /etc/init.d/cpuspeed
	test -f /etc/default/cpuspeed || cp cpuspeed.conf /etc/default/cpuspeed
	update-rc.d cpuspeed start 40 S .
	/etc/init.d/cpuspeed restart 

clean:
	rm -f *.o $(TARGET) $(DEBUG_TARGET)

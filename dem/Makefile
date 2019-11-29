F51 := F51.zip
HGT := N23E120.hgt

all: $(HGT)

$(F51):
	wget http://viewfinderpanoramas.org/dem3/F51.zip

$(HGT): $(F51)
	unzip $(F51)
	mv F51/* .
	rmdir F51

.PHONY: all

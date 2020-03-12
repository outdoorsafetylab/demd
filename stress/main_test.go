package main

import "testing"

func TestRandLoc(t *testing.T) {
	for i := 0; i < 1000; i++ {
		lat, lon := randLoc()
		if lat < latMin || lat > latMax {
			t.Fatalf("Lat out of range: %f", lat)
		}
		if lon < lonMin || lon > lonMax {
			t.Fatalf("Lon out of range: %f", lon)
		}
	}
}

package main

import (
	"bytes"
	"errors"
	"flag"
	"fmt"
	"io/ioutil"
	"log"
	"math/rand"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"time"
)

const (
	latMin = 21.0
	latMax = 23.0
	lonMin = 121.0
	lonMax = 123.0
)

var (
	host               string
	clients            int
	requestsPerClient  int
	locationPerRequest int
	auth               string
)

func main() {
	flag.StringVar(&host, "h", "http://127.0.0.1:8082", "base URL of test target")
	flag.StringVar(&auth, "a", "", "'Authorization' header")
	flag.IntVar(&clients, "c", 50, "number of clients")
	flag.IntVar(&requestsPerClient, "r", 50, "requests per client")
	flag.IntVar(&locationPerRequest, "l", 1000, "locations per request")
	flag.Usage = usage
	flag.Parse()
	if host == "" {
		usage()
	}
	url := host + "/v1/elevations"
	errCh := make(chan error)
	countCh := make(chan int)
	durationCh := make(chan time.Duration)
	total := clients * requestsPerClient * locationPerRequest
	count := 0
	var duration time.Duration
	log.Printf("Testing: %s", host)
	log.Printf("Number of clients: %d", clients)
	log.Printf("Requests per client: %d", requestsPerClient)
	log.Printf("Locations per request: %d", locationPerRequest)
	t0 := time.Now()
	for i := 0; i < clients; i++ {
		c := &client{
			client: &http.Client{},
			url:    url,
		}
		go func(c *client) {
			for i := 0; i < requestsPerClient; i++ {
				n := locationPerRequest
				t0 := time.Now()
				err := c.query(n)
				if err != nil {
					log.Printf("Failed to query for %dth request: %s", i+1, err.Error())
					errCh <- err
					return
				}
				durationCh <- time.Now().Sub(t0)
				countCh <- n
			}
		}(c)
	}
	for {
		select {
		case n := <-countCh:
			count += n
		case dt := <-durationCh:
			duration += dt
		case <-errCh:
			log.Printf("Aborted at %d queries.", count)
			os.Exit(1)
		}
		if count >= total {
			log.Printf("Finished %d elevation queries by %d requests.", total, clients*requestsPerClient)
			break
		}
	}
	dt := time.Now().Sub(t0)
	log.Printf("Time elapsed: %v", dt)
	log.Printf("Average RTT per request: %v", duration/time.Duration(clients*requestsPerClient))
	log.Printf("Average throughput: %d elevations/sec", time.Second*time.Duration(total)/dt)
}

func usage() {
	fmt.Printf("Usage: %s\n", filepath.Base(os.Args[0]))
	os.Exit(1)
}

type client struct {
	client *http.Client
	url    string
}

func (c *client) query(n int) error {
	req, err := http.NewRequest("POST", c.url, bytes.NewBuffer([]byte(randLocs(n))))
	if err != nil {
		return err
	}
	req.Header.Set("Content-Type", "application/json")
	if auth != "" {
		req.Header.Set("Authorization", auth)
	}
	res, err := c.client.Do(req)
	if err != nil {
		return err
	}
	defer res.Body.Close()
	_, err = ioutil.ReadAll(res.Body)
	if err != nil {
		return err
	}
	if res.StatusCode != 200 {
		return errors.New(res.Status)
	}
	return nil
}

// we don't use json.Marshal() here to maximize client strenth
func randLocs(n int) string {
	locs := make([]string, n)
	for i := 0; i < n; i++ {
		locs[i] = randLocString()
	}
	return "[" + strings.Join(locs, ",") + "]"
}

func randLocString() string {
	lat, lon := randLoc()
	return fmt.Sprintf("[%f,%f]", lon, lat)
}

func randLoc() (float64, float64) {
	lat := latMin + rand.Float64()*(latMax-latMin)
	lon := lonMin + rand.Float64()*(lonMax-lonMin)
	return lat, lon
}

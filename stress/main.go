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
)

func main() {
	flag.StringVar(&host, "h", "http://127.0.0.1:8082", "base URL of test target")
	flag.IntVar(&clients, "c", 5, "number of clients")
	flag.IntVar(&requestsPerClient, "r", 500, "requests per client")
	flag.IntVar(&locationPerRequest, "l", 1000, "locations per request")
	flag.Usage = usage
	flag.Parse()
	if host == "" {
		usage()
	}
	url := host + "/v1/elevations"
	errCh := make(chan error)
	countCh := make(chan int)
	total := clients * requestsPerClient * locationPerRequest
	count := 0
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
				err := c.query(n)
				if err != nil {
					log.Printf("Failed to query for %dth request: %s", i+1, err.Error())
					errCh <- err
					return
				}
				countCh <- n
			}
		}(c)
	}
	for {
		select {
		case n := <-countCh:
			count += n
		case <-errCh:
			log.Printf("Aborted at %d queries.", count)
			os.Exit(1)
		}
		if count >= total {
			log.Printf("Finished %d queries.", total)
			break
		}
	}
	dt := time.Now().Sub(t0)
	log.Printf("Average time per elevation: %v", dt/time.Duration(total))
	log.Printf("Throughput: %d elevations/sec", time.Second*time.Duration(total)/dt)
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
	res, err := c.client.Post(c.url, "application/json", bytes.NewBuffer([]byte(randLocs(n))))
	if err != nil {
		return err
	}
	defer res.Body.Close()
	defer ioutil.ReadAll(res.Body)
	if res.StatusCode != 200 {
		return errors.New(res.Status)
	}
	return nil
}

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

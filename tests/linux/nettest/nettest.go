// nettest: the second test of KolibriOS's Linux personality: files in the
// RAM file system, Go's own DNS resolver over UDP, TCP with TLS (the CA
// bundle from the ramdisk), concurrent connections through epoll.
package main

import (
	"crypto/tls"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"sync"
	"time"
)

func step(name string, err error) {
	if err != nil {
		fmt.Printf("nettest: %s: FAILED: %v\n", name, err)
		os.Exit(1)
	}
}

func main() {
	// files
	step("mkdir", os.MkdirAll("/home/user/.local/share/nettest", 0700))
	f := "/home/user/.local/share/nettest/data"
	step("write", os.WriteFile(f, []byte("kolibri"), 0600))
	b, err := os.ReadFile(f)
	step("read", err)
	fmt.Printf("nettest: file: %q\n", b)

	// DNS
	t0 := time.Now()
	addrs, err := net.LookupHost("example.com")
	step("dns", err)
	fmt.Printf("nettest: example.com: %v (%v)\n", addrs, time.Since(t0).Round(time.Millisecond))

	// TLS, several at once
	hosts := []string{"https://example.com/", "https://web.whatsapp.com/", "https://telegram.org/"}
	var wg sync.WaitGroup
	c := &http.Client{Timeout: 30 * time.Second}
	for _, u := range hosts {
		wg.Add(1)
		go func(u string) {
			defer wg.Done()
			t0 := time.Now()
			r, err := c.Get(u)
			if err != nil {
				fmt.Printf("nettest: %s: FAILED: %v\n", u, err)
				return
			}
			n, _ := io.Copy(io.Discard, r.Body)
			r.Body.Close()
			v := "?"
			if r.TLS != nil {
				v = tls.VersionName(r.TLS.Version)
			}
			fmt.Printf("nettest: %s: %s, %s, %d bytes (%v)\n", u, r.Status, v, n,
				time.Since(t0).Round(time.Millisecond))
		}(u)
	}
	wg.Wait()
	fmt.Println("nettest: done")
}

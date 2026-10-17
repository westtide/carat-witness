package main

// Minimal RFC 6455 WebSocket server (text frames only), standard library
// only. The dashboard receives JSON snapshots; it never sends data we need
// to interpret beyond control frames.

import (
	"bufio"
	"crypto/sha1"
	"encoding/base64"
	"encoding/binary"
	"errors"
	"io"
	"net"
	"net/http"
	"strings"
	"sync"
)

const wsGUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

type wsConn struct {
	conn net.Conn
	wmu  sync.Mutex
	br   *bufio.Reader
}

func upgradeWS(w http.ResponseWriter, req *http.Request) (*wsConn, error) {
	key := req.Header.Get("Sec-WebSocket-Key")
	if key == "" || !strings.EqualFold(req.Header.Get("Upgrade"), "websocket") {
		http.Error(w, "not a websocket handshake", http.StatusBadRequest)
		return nil, errors.New("bad handshake")
	}
	h := sha1.Sum([]byte(key + wsGUID))
	accept := base64.StdEncoding.EncodeToString(h[:])

	hj, ok := w.(http.Hijacker)
	if !ok {
		http.Error(w, "hijack unsupported", http.StatusInternalServerError)
		return nil, errors.New("hijack unsupported")
	}
	conn, brw, err := hj.Hijack()
	if err != nil {
		return nil, err
	}
	resp := "HTTP/1.1 101 Switching Protocols\r\n" +
		"Upgrade: websocket\r\nConnection: Upgrade\r\n" +
		"Sec-WebSocket-Accept: " + accept + "\r\n\r\n"
	if _, err := conn.Write([]byte(resp)); err != nil {
		conn.Close()
		return nil, err
	}
	ws := &wsConn{conn: conn, br: brw.Reader}
	go ws.readLoop()
	return ws, nil
}

// readLoop consumes (and ignores) client frames; exits on close/error.
func (w *wsConn) readLoop() {
	for {
		var hdr [2]byte
		if _, err := io.ReadFull(w.br, hdr[:]); err != nil {
			w.conn.Close()
			return
		}
		opcode := hdr[0] & 0x0F
		masked := hdr[1]&0x80 != 0
		length := uint64(hdr[1] & 0x7F)
		switch length {
		case 126:
			var ext [2]byte
			if _, err := io.ReadFull(w.br, ext[:]); err != nil {
				w.conn.Close()
				return
			}
			length = uint64(binary.BigEndian.Uint16(ext[:]))
		case 127:
			var ext [8]byte
			if _, err := io.ReadFull(w.br, ext[:]); err != nil {
				w.conn.Close()
				return
			}
			length = binary.BigEndian.Uint64(ext[:])
		}
		if length > 1<<20 {
			w.conn.Close()
			return
		}
		var maskKey [4]byte
		if masked {
			if _, err := io.ReadFull(w.br, maskKey[:]); err != nil {
				w.conn.Close()
				return
			}
		}
		if length > 0 {
			if _, err := io.CopyN(io.Discard, w.br, int64(length)); err != nil {
				w.conn.Close()
				return
			}
		}
		if opcode == 0x8 { // close
			w.writeFrame(0x88, nil) // echo close
			w.conn.Close()
			return
		}
	}
}

// writeFrame sends one server->client frame (no masking, as required).
func (w *wsConn) writeFrame(opcode byte, payload []byte) error {
	w.wmu.Lock()
	defer w.wmu.Unlock()
	var hdr [10]byte
	hdr[0] = 0x80 | opcode // FIN + opcode
	n := len(payload)
	switch {
	case n < 126:
		hdr[1] = byte(n)
		if _, err := w.conn.Write(hdr[:2]); err != nil {
			return err
		}
	case n < 65536:
		hdr[1] = 126
		binary.BigEndian.PutUint16(hdr[2:], uint16(n))
		if _, err := w.conn.Write(hdr[:4]); err != nil {
			return err
		}
	default:
		hdr[1] = 127
		binary.BigEndian.PutUint64(hdr[2:], uint64(n))
		if _, err := w.conn.Write(hdr[:10]); err != nil {
			return err
		}
	}
	_, err := w.conn.Write(payload)
	return err
}

func (w *wsConn) writeText(b []byte) error { return w.writeFrame(0x1, b) }

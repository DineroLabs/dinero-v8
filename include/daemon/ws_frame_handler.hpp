#pragma once
#include "compat/net_compat.h"
#ifndef _WIN32
#include <poll.h>
#else
#include <winsock2.h>
#endif
#include <errno.h>
#include <cstring>
#include <vector>
#include <iostream>

// Frame size limits for security
static constexpr size_t MAX_FRAME_SIZE = 1024 * 1024; // 1MB max frame

// WebSocket frame structure (compatible with existing code)
struct WsFrame { 
    uint8_t opcode; 
    std::vector<uint8_t> payload; 
    bool fin; 
};

// Robust WebSocket frame reading that handles EAGAIN correctly
inline bool ws_recv_frame(int fd, WsFrame& out) {
    // Read frame header (at least 2 bytes)
    uint8_t header[14]; // Max header size
    ssize_t n = recv(fd, reinterpret_cast<char*>(header), 2, 0);
    
    if (n == 0) {
        // Peer closed connection
        std::cout << "[ws] Peer closed connection fd=" << fd << std::endl;
        return false;
    }
    
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            // No data available yet - this is NOT an error!
            std::cout << "[ws] No data available (EAGAIN), continuing fd=" << fd << std::endl;
            return true; // Return true to keep connection alive
        }
        // Real error
        std::cout << "[ws] recv error fd=" << fd << ": " << strerror(errno) << std::endl;
        return false;
    }
    
    if (n < 2) {
        std::cout << "[ws] Incomplete header, need more data fd=" << fd << std::endl;
        return true; // Keep trying
    }
    
    // Parse frame header
    out.fin = (header[0] & 0x80) != 0;
    out.opcode = header[0] & 0x0F;
    bool masked = (header[1] & 0x80) != 0;
    uint64_t payload_len = header[1] & 0x7F;
    
    size_t header_len = 2;
    
    // Extended payload length
    if (payload_len == 126) {
        n = recv(fd, reinterpret_cast<char*>(header + 2), 2, 0);
        if (n < 2) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;
            return false;
        }
        payload_len = (header[2] << 8) | header[3];
        header_len = 4;
    } else if (payload_len == 127) {
        n = recv(fd, reinterpret_cast<char*>(header + 2), 8, 0);
        if (n < 8) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;
            return false;
        }
        payload_len = 0;
        for (int i = 0; i < 8; i++) {
            payload_len = (payload_len << 8) | header[2 + i];
        }
        header_len = 10;
    }
    
    // Security: Check frame size limit
    if (payload_len > MAX_FRAME_SIZE) {
        std::cout << "[ws] Frame too large: " << payload_len << " bytes, max: " << MAX_FRAME_SIZE << " fd=" << fd << std::endl;
        return false; // Will trigger close with code 1009
    }
    
    // Read mask if present
    uint8_t mask[4] = {0};
    if (masked) {
        n = recv(fd, reinterpret_cast<char*>(mask), 4, 0);
        if (n < 4) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;
            return false;
        }
    }
    
    // Read payload
    out.payload.resize(payload_len);
    if (payload_len > 0) {
        size_t total_read = 0;
        while (total_read < payload_len) {
            n = recv(fd, reinterpret_cast<char*>(out.payload.data() + total_read), static_cast<int>(payload_len - total_read), 0);
            if (n == 0) return false; // Peer closed
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) continue; // Keep trying
                return false; // Real error
            }
            total_read += n;
        }
        
        // Unmask payload if needed
        if (masked) {
            for (size_t i = 0; i < payload_len; i++) {
                out.payload[i] ^= mask[i % 4];
            }
        }
    }
    
    std::cout << "[ws] Successfully read frame: opcode=" << (int)out.opcode 
              << ", payload_len=" << payload_len << ", fd=" << fd << std::endl;
    
    return true;
}

// Correct server→client frame builder (unmasked, proper lengths)
inline void write_u16_be(std::vector<uint8_t>& v, uint16_t x) {
  v.push_back(uint8_t((x >> 8) & 0xFF));
  v.push_back(uint8_t(x & 0xFF));
}

inline void write_u64_be(std::vector<uint8_t>& v, uint64_t x) {
  for (int i = 7; i >= 0; --i) v.push_back(uint8_t((x >> (8*i)) & 0xFF));
}

inline std::vector<uint8_t> make_ws_text_frame_server(const std::string& text) {
  std::vector<uint8_t> f;
  const uint64_t len = text.size();

  // byte0: FIN=1 (0x80) | opcode=1 (text)
  f.push_back(0x80 | 0x01);

  // byte1: mask=0 for server frames; then length selector
  if (len <= 125) {
    f.push_back(uint8_t(len));            // mask bit 0
  } else if (len <= 0xFFFF) {
    f.push_back(126);
    write_u16_be(f, uint16_t(len));
  } else {
    f.push_back(127);
    write_u64_be(f, len);
  }

  // payload (UTF-8)
  f.insert(f.end(), text.begin(), text.end());
  
  std::cout << "[ws] Built frame: b0=0x" << std::hex << (int)f[0] 
            << ", b1=0x" << (int)f[1] << ", len=" << std::dec << len 
            << ", total=" << f.size() << " bytes" << std::endl;
  
  return f;
}

// Enhanced send-all wrapper with EPIPE/ECONNRESET handling
inline bool send_all_ws(int fd, const uint8_t* buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = ::send(fd, reinterpret_cast<const char*>(buf + off), static_cast<int>(len - off), 0);
        if (n > 0) { 
            off += size_t(n); 
            continue; 
        }
        if (n == -1 && (errno == EINTR)) continue;
        if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
#ifdef _WIN32
            WSAPOLLFD p{(SOCKET)fd, POLLOUT, 0};
            if (WSAPoll(&p, 1, 5000) <= 0) return false;
#else
            pollfd p{fd, POLLOUT, 0};
            if (poll(&p, 1, 5000) <= 0) return false;
#endif
            continue;
        }
        // Handle connection errors as immediate cleanup
        if (n == -1 && (errno == EPIPE || errno == ECONNRESET)) {
            std::cout << "[ws] Connection error (EPIPE/ECONNRESET), immediate cleanup fd=" << fd << std::endl;
            return false; // Trigger immediate cleanup
        }
        return false; // hard error
    }
    return true;
}

// Replace the existing ws_send_text with the robust version
inline bool ws_send_text(int fd, const std::string& s) {
  auto frame = make_ws_text_frame_server(s);
  bool success = send_all_ws(fd, frame.data(), frame.size());
  
  std::cout << "[ws] Sent text frame: " << s.length() << " bytes, success=" << success 
            << ", fd=" << fd << std::endl;
  
  return success;
}

// Send pong frame
inline bool ws_send_pong(int fd, const uint8_t* data, size_t len) {
    std::vector<uint8_t> frame;
    frame.push_back(0x8A); // FIN=1, opcode=0xA (pong)
    
    if (len < 126) {
        frame.push_back(static_cast<uint8_t>(len));
    } else {
        // Pong frames should be small, but handle it anyway
        frame.push_back(126);
        frame.push_back((len >> 8) & 0xFF);
        frame.push_back(len & 0xFF);
    }
    
    frame.insert(frame.end(), data, data + len);
    
    ssize_t sent = send(fd, reinterpret_cast<const char*>(frame.data()), static_cast<int>(frame.size()), 0);
    return (sent == static_cast<ssize_t>(frame.size()));
}

// Send close frame
inline bool ws_send_close(int fd, uint16_t code = 1000, const char* reason = "", size_t rlen = 0) {
    std::vector<uint8_t> frame;
    frame.push_back(0x88); // FIN=1, opcode=0x8 (close)
    
    size_t payload_len = 2 + rlen; // 2 bytes for code + reason length
    frame.push_back(static_cast<uint8_t>(payload_len));
    
    // Close code (big-endian)
    frame.push_back((code >> 8) & 0xFF);
    frame.push_back(code & 0xFF);
    
    // Reason
    if (rlen > 0) {
        frame.insert(frame.end(), reason, reason + rlen);
    }
    
    ssize_t sent = send(fd, reinterpret_cast<const char*>(frame.data()), static_cast<int>(frame.size()), 0);
    std::cout << "[ws] Sent close frame: code=" << code << ", fd=" << fd << std::endl;
    
    return (sent == static_cast<ssize_t>(frame.size()));
}

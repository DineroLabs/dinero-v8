#!/usr/bin/env python3
"""
Dinero DNS Seeder
Crawls the network and provides DNS responses for peer discovery
"""

import asyncio
import os
import socket
import struct
import time
import json
import logging
from typing import Dict, List, Set, Tuple
from dataclasses import dataclass, asdict
from datetime import datetime, timedelta
import dnslib
from dnslib.server import DNSServer, DNSHandler, BaseResolver

# Configure logging
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(levelname)s - %(message)s'
)
logger = logging.getLogger(__name__)

@dataclass
class PeerInfo:
    """Information about a network peer"""
    ip: str
    port: int
    last_seen: datetime
    version: int = 0
    services: int = 0
    user_agent: str = ""
    height: int = 0
    is_reachable: bool = False
    response_time: float = 0.0

class DineroCoinProtocol:
    """Dinero P2P protocol implementation for peer discovery"""

    MAINNET_MAGIC = 0xD1A0C0DE
    MAGIC_BYTES = struct.pack('<I', int(os.environ.get("NETWORK_MAGIC", str(MAINNET_MAGIC)), 0))
    VERSION = int(os.environ.get("PROTOCOL_VERSION", "70016"))
    NODE_NETWORK = 1 << 0
    NODE_UTREEXO = 1 << 24
    NODE_UTREEXO_BRIDGE = 1 << 25
    SERVICES = NODE_NETWORK | NODE_UTREEXO | NODE_UTREEXO_BRIDGE
    DEFAULT_PORT = int(os.environ.get("P2P_PORT", "20999"))
    USER_AGENT = b"/dinero-dns-seeder:0.1/"
    
    @staticmethod
    def create_version_message(peer_ip: str, peer_port: int) -> bytes:
        """Create a version message for peer handshake"""
        timestamp = int(time.time())
        
        # Build version message payload
        payload = struct.pack('<I', DineroCoinProtocol.VERSION)  # version
        payload += struct.pack('<Q', DineroCoinProtocol.SERVICES)  # services
        payload += struct.pack('<Q', timestamp)  # timestamp
        
        # addr_recv (peer address)
        payload += struct.pack('<Q', DineroCoinProtocol.SERVICES)  # services
        payload += b'\x00' * 10 + b'\xff\xff'  # IPv4-mapped IPv6 prefix
        payload += socket.inet_aton(peer_ip)  # IP
        payload += struct.pack('>H', peer_port)  # port (big-endian)
        
        # addr_from (our address - can be zeros)
        payload += struct.pack('<Q', 0)  # services
        payload += b'\x00' * 16  # IP (zeros)
        payload += struct.pack('>H', 0)  # port
        
        payload += struct.pack('<Q', 0)  # nonce
        payload += bytes([len(DineroCoinProtocol.USER_AGENT)])
        payload += DineroCoinProtocol.USER_AGENT
        payload += struct.pack('<I', 0)  # start_height
        payload += b'\x01'  # relay
        
        return DineroCoinProtocol._create_message(b'version', payload)
    
    @staticmethod
    def _create_message(command: bytes, payload: bytes) -> bytes:
        """Create a P2P protocol message"""
        # Header: magic + command + length + checksum
        header = DineroCoinProtocol.MAGIC_BYTES
        header += command.ljust(12, b'\x00')  # 12-byte command
        header += struct.pack('<I', len(payload))  # payload length
        
        # Checksum (first 4 bytes of double SHA256)
        import hashlib
        checksum = hashlib.sha256(hashlib.sha256(payload).digest()).digest()[:4]
        header += checksum
        
        return header + payload

class NetworkCrawler:
    """Crawls the Dinero network to discover active peers"""
    
    def __init__(self):
        self.peers: Dict[str, PeerInfo] = {}
        port = DineroCoinProtocol.DEFAULT_PORT
        self.seed_peers = [
            ('seed1.dinero-coin.com', port),
            ('seed2.dinero-coin.com', port),
            ('seed3.dinero-coin.com', port),
            ('seed4.dinero-coin.com', port),
        ]
        self.crawl_interval = 300  # 5 minutes
        self.peer_timeout = 3600   # 1 hour
    
    async def start_crawling(self):
        """Start the network crawling process"""
        logger.info("Starting network crawler...")
        
        while True:
            try:
                await self._crawl_network()
                await self._cleanup_stale_peers()
                await asyncio.sleep(self.crawl_interval)
            except Exception as e:
                logger.error(f"Crawling error: {e}")
                await asyncio.sleep(60)
    
    async def _crawl_network(self):
        """Crawl the network for active peers"""
        logger.info("Starting network crawl...")
        
        # Start with seed peers
        tasks = []
        for host, port in self.seed_peers:
            try:
                ip = socket.gethostbyname(host)
                tasks.append(self._check_peer(ip, port))
            except socket.gaierror:
                logger.warning(f"Could not resolve {host}")
        
        # Check existing peers
        for peer_key, peer in list(self.peers.items()):
            tasks.append(self._check_peer(peer.ip, peer.port))
        
        # Execute all checks concurrently
        if tasks:
            results = await asyncio.gather(*tasks, return_exceptions=True)
            active_count = sum(1 for r in results if r and not isinstance(r, Exception))
            logger.info(f"Crawl complete: {active_count}/{len(tasks)} peers active")
    
    async def _check_peer(self, ip: str, port: int) -> bool:
        """Check if a peer is reachable and get its info"""
        peer_key = f"{ip}:{port}"
        start_time = time.time()
        
        try:
            # Attempt TCP connection
            reader, writer = await asyncio.wait_for(
                asyncio.open_connection(ip, port),
                timeout=10.0
            )
            
            # Send version message
            version_msg = DineroCoinProtocol.create_version_message(ip, port)
            writer.write(version_msg)
            await writer.drain()
            
            # Read response (simplified - just check if we get data)
            response = await asyncio.wait_for(reader.read(1024), timeout=5.0)
            
            writer.close()
            await writer.wait_closed()
            
            response_time = time.time() - start_time
            
            # Update peer info
            peer_info = PeerInfo(
                ip=ip,
                port=port,
                last_seen=datetime.now(),
                is_reachable=True,
                response_time=response_time
            )
            
            self.peers[peer_key] = peer_info
            logger.debug(f"Peer {peer_key} is reachable ({response_time:.2f}s)")
            return True
            
        except Exception as e:
            # Mark peer as unreachable
            if peer_key in self.peers:
                self.peers[peer_key].is_reachable = False
            logger.debug(f"Peer {peer_key} unreachable: {e}")
            return False
    
    async def _cleanup_stale_peers(self):
        """Remove peers that haven't been seen recently"""
        cutoff = datetime.now() - timedelta(seconds=self.peer_timeout)
        stale_peers = [
            key for key, peer in self.peers.items()
            if peer.last_seen < cutoff
        ]
        
        for key in stale_peers:
            del self.peers[key]
        
        if stale_peers:
            logger.info(f"Removed {len(stale_peers)} stale peers")
    
    def get_active_peers(self, max_peers: int = 25) -> List[str]:
        """Get list of active peer IPs for DNS responses"""
        active_peers = [
            peer.ip for peer in self.peers.values()
            if peer.is_reachable and peer.last_seen > datetime.now() - timedelta(minutes=30)
        ]
        
        # Sort by response time and return best peers
        sorted_peers = sorted(
            [
                (ip, self.peers[f"{ip}:{DineroCoinProtocol.DEFAULT_PORT}"].response_time)
                for ip in active_peers
                if f"{ip}:{DineroCoinProtocol.DEFAULT_PORT}" in self.peers
            ],
            key=lambda x: x[1]
        )
        
        return [ip for ip, _ in sorted_peers[:max_peers]]

class DineroCoinDNSResolver(BaseResolver):
    """DNS resolver for Dinero seed queries"""
    
    def __init__(self, crawler: NetworkCrawler):
        self.crawler = crawler
        self.domains = {
            os.environ.get("SEEDER_DOMAIN", "seed.dinero-coin.com"),
            'seed.dinero-coin.com',
            'dnsseed.dinero-coin.com',
            'seeds.dinero-coin.com',
        }
    
    def resolve(self, request, handler):
        """Resolve DNS queries for seed domains"""
        reply = request.reply()
        qname = str(request.q.qname).rstrip('.')
        qtype = request.q.qtype
        
        logger.debug(f"DNS query: {qname} {dnslib.QTYPE[qtype]}")
        
        if qname in self.domains:
            if qtype == dnslib.QTYPE.A:
                # Return IPv4 addresses of active peers
                active_peers = self.crawler.get_active_peers(25)
                for ip in active_peers:
                    try:
                        # Validate IPv4
                        socket.inet_aton(ip)
                        reply.add_answer(
                            dnslib.RR(qname, dnslib.QTYPE.A, rdata=dnslib.A(ip), ttl=300)
                        )
                    except socket.error:
                        continue
                
                logger.info(f"Returned {len(reply.rr)} A records for {qname}")
            
            elif qtype == dnslib.QTYPE.AAAA:
                # IPv6 support (if available)
                # For now, return empty response
                pass
        
        return reply

async def main():
    """Main DNS seeder application"""
    logger.info("🌱 Starting Dinero DNS Seeder")
    
    # Initialize network crawler
    crawler = NetworkCrawler()
    
    # Start crawler in background
    crawler_task = asyncio.create_task(crawler.start_crawling())
    
    # Initialize DNS server
    resolver = DineroCoinDNSResolver(crawler)
    dns_server = DNSServer(resolver, port=53, address="0.0.0.0")
    
    logger.info("DNS seeder started on port 53")
    logger.info(
        "Dinero P2P constants: magic=0x%08x protocol=%d port=%d",
        int.from_bytes(DineroCoinProtocol.MAGIC_BYTES, byteorder="little"),
        DineroCoinProtocol.VERSION,
        DineroCoinProtocol.DEFAULT_PORT,
    )
    logger.info("Seeding domains: %s", ", ".join(sorted(resolver.domains)))
    
    try:
        # Start DNS server
        dns_server.start_thread()
        
        # Keep running
        await crawler_task
        
    except KeyboardInterrupt:
        logger.info("Shutting down DNS seeder...")
        dns_server.stop()
        crawler_task.cancel()
    
    except Exception as e:
        logger.error(f"DNS seeder error: {e}")
        raise

if __name__ == "__main__":
    asyncio.run(main())

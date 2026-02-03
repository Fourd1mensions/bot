#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Lemmatizer microservice using pymorphy2 for Russian language.
Listens on a Unix socket and processes lemmatization requests.

Usage:
    python3 lemmatizer_service.py [--socket /tmp/lemmatizer.sock]

Request format (JSON):
    {"words": ["слово1", "слово2", ...]}

Response format (JSON):
    {"lemmas": ["лемма1", "лемма2", ...]}
"""

import argparse
import json
import os
import signal
import socket
import sys
import logging

try:
    from pymorphy3 import MorphAnalyzer
except ImportError:
    try:
        from pymorphy2 import MorphAnalyzer
    except ImportError:
        print("Error: pymorphy3/pymorphy2 not installed. Install with: pip install pymorphy3", file=sys.stderr)
        sys.exit(1)

# Configure logging
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s [%(levelname)s] %(message)s',
    datefmt='%Y-%m-%d %H:%M:%S'
)
logger = logging.getLogger(__name__)

# Global morph analyzer (lazy initialized)
morph = None

def get_analyzer():
    """Get or create the morphological analyzer."""
    global morph
    if morph is None:
        logger.info("Initializing pymorphy3 analyzer...")
        morph = MorphAnalyzer()
        logger.info("Analyzer initialized")
    return morph

def lemmatize_word(word: str) -> str:
    """Lemmatize a single word."""
    try:
        analyzer = get_analyzer()
        parsed = analyzer.parse(word)
        if parsed:
            return parsed[0].normal_form
        return word
    except Exception as e:
        logger.error(f"Error lemmatizing word '{word}': {e}")
        return word

def lemmatize_words(words: list) -> list:
    """Lemmatize a list of words."""
    return [lemmatize_word(w) for w in words]

def handle_request(data: bytes) -> bytes:
    """Handle a single request and return response."""
    try:
        request = json.loads(data.decode('utf-8'))
        words = request.get('words', [])

        if not isinstance(words, list):
            return json.dumps({'error': 'words must be a list'}).encode('utf-8')

        lemmas = lemmatize_words(words)
        response = {'lemmas': lemmas}
        return json.dumps(response, ensure_ascii=False).encode('utf-8')

    except json.JSONDecodeError as e:
        return json.dumps({'error': f'Invalid JSON: {e}'}).encode('utf-8')
    except Exception as e:
        logger.error(f"Error handling request: {e}")
        return json.dumps({'error': str(e)}).encode('utf-8')

def handle_client(client_socket):
    """Handle a client connection."""
    try:
        # Read data (expect single request per connection)
        data = b''
        while True:
            chunk = client_socket.recv(4096)
            if not chunk:
                break
            data += chunk
            # Check for complete JSON (simple heuristic: ends with })
            if data.strip().endswith(b'}'):
                break

        if data:
            response = handle_request(data)
            client_socket.sendall(response)
    except Exception as e:
        logger.error(f"Error handling client: {e}")
    finally:
        client_socket.close()

def cleanup_socket(socket_path: str):
    """Remove socket file if it exists."""
    try:
        if os.path.exists(socket_path):
            os.unlink(socket_path)
    except OSError as e:
        logger.warning(f"Could not remove socket file: {e}")

def run_server(socket_path: str):
    """Run the Unix socket server."""
    # Cleanup any existing socket
    cleanup_socket(socket_path)

    # Create Unix socket
    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)

    try:
        server.bind(socket_path)
        os.chmod(socket_path, 0o666)  # Allow other users to connect
        server.listen(5)

        logger.info(f"Lemmatizer service listening on {socket_path}")

        # Initialize analyzer on startup
        get_analyzer()

        while True:
            try:
                client, _ = server.accept()
                handle_client(client)
            except KeyboardInterrupt:
                break
            except Exception as e:
                logger.error(f"Error accepting connection: {e}")

    finally:
        server.close()
        cleanup_socket(socket_path)
        logger.info("Server stopped")

def signal_handler(signum, frame):
    """Handle shutdown signals."""
    logger.info(f"Received signal {signum}, shutting down...")
    sys.exit(0)

def main():
    parser = argparse.ArgumentParser(description='Lemmatizer microservice using pymorphy2')
    parser.add_argument('--socket', default='/tmp/lemmatizer.sock',
                       help='Unix socket path (default: /tmp/lemmatizer.sock)')
    parser.add_argument('--test', action='store_true',
                       help='Run a quick test and exit')
    args = parser.parse_args()

    if args.test:
        # Quick test mode
        test_words = ['красивая', 'картинка', 'играет', 'музыка', 'хорошо']
        print(f"Testing lemmatization:")
        print(f"Input:  {test_words}")
        lemmas = lemmatize_words(test_words)
        print(f"Output: {lemmas}")
        return

    # Register signal handlers
    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)

    run_server(args.socket)

if __name__ == '__main__':
    main()

FROM python:3.11-slim

LABEL maintainer="Dinero Team"
LABEL description="Dinero Global Node Registry"
LABEL version="1.0"

# Install dependencies
RUN pip install --no-cache-dir requests

# Create app directory
WORKDIR /app

# Copy registry files
COPY dinero_registry.py dinero_registry_extended.py ./
RUN chmod +x dinero_registry.py dinero_registry_extended.py

# Create non-root user
RUN useradd -r -u 1000 -s /bin/false dinero && \
    chown -R dinero:dinero /app

USER dinero

# Expose HTTP port
EXPOSE 8080

# Health check
HEALTHCHECK --interval=60s --timeout=5s --start-period=10s --retries=3 \
    CMD python3 -c "import requests; requests.get('http://localhost:8080/api/status').raise_for_status()"

# Default to basic registry
CMD ["python3", "dinero_registry.py", "--port", "8080"]

# To use extended version with registration:
# docker run -d -p 8080:8080 dinero-registry python3 dinero_registry_extended.py --port 8080

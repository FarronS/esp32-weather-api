FROM python:3.12-slim

WORKDIR /app
COPY weather_relay.py .

ENV HOST=0.0.0.0 \
    PORT=8080 \
    LAT=54.1961 \
    LON=37.6182 \
    TZ_OFFSET=3 \
    CITY=Тула \
    REFRESH=600

EXPOSE 8080

HEALTHCHECK --interval=30s --timeout=5s --start-period=20s \
    CMD python -c "import urllib.request;urllib.request.urlopen('http://127.0.0.1:8080/health', timeout=4)"

CMD ["python", "weather_relay.py"]

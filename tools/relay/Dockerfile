# The System Link relay as a container: build from the repository root:  docker build -f tools/relay/Dockerfile -t reforged-relay .
FROM python:3.12-slim
WORKDIR /app
COPY tools/relay.py /app/relay.py
USER nobody
EXPOSE 3075/udp
ENTRYPOINT ["python", "-u", "/app/relay.py"]
CMD ["--port", "3075"]

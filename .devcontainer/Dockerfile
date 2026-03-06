FROM centos:7

# Switch to vault repos (CentOS 7 is EOL)
RUN sed -i 's/^mirrorlist/#mirrorlist/' /etc/yum.repos.d/*.repo && \
    sed -i 's|^#baseurl=http://mirror.centos.org|baseurl=http://vault.centos.org|' /etc/yum.repos.d/*.repo

# Install development tools
RUN yum install -y \
    gcc \
    gcc-c++ \
    make \
    gdb \
    strace \
    wget \
    unzip \
    && yum clean all

# Create workspace
WORKDIR /workspace

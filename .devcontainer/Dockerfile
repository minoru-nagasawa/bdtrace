FROM centos:5

# Switch to vault repos (CentOS 5 is EOL)
RUN sed -i 's/^mirrorlist/#mirrorlist/' /etc/yum.repos.d/*.repo && \
    sed -i 's|^#baseurl=http://mirror.centos.org/centos/$releasever|baseurl=http://vault.centos.org/5.11|' /etc/yum.repos.d/*.repo

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

ARG IMGTAG
FROM ${IMGTAG}
ARG REQUIREMENTS

ADD certificate_authorities /etc/pki/ca-trust/source/anchors/
RUN update-ca-trust

ADD withproxy /opt/bootstrap/
ADD bootstrap/.curlrc /root/

RUN /opt/bootstrap/withproxy yum install -y python3-devel openssh-clients flex

RUN rpm -Uvh --nodeps $(repoquery --location dropbear)
RUN rpm -Uvh --nodeps $(repoquery --location libtommath)
RUN rpm -Uvh --nodeps $(repoquery --location libtomcrypt)

ENV PATH="/opt/rh/devtoolset-10/root/usr/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:{$PATH}:/apps/research/tools/bin"
ENV VCPKG_DEFAULT_BINARY_CACHE=/"scratch/data/vcpkg_cache"

# CLion stuff
ADD bootstrap/install_dev_tools.sh /opt/bootstrap/
ADD bootstrap/cxx_profile.sh /opt/bootstrap/
RUN /opt/bootstrap/install_dev_tools.sh

# git completion
RUN curl https://raw.githubusercontent.com/git/git/master/contrib/completion/git-completion.bash -o /root/.git-completion.bash

ADD .arcticdbbashrc /root/.arcticdbbashrc
RUN echo "source /root/.arcticdbbashrc" >> /root/.bashrc

# Python dependencies for developing in the container
RUN update-alternatives --install /usr/bin/python python /usr/bin/python3.6 1
RUN mkdir -p /root/pyenvs/dev
RUN python -m venv /root/pyenvs/dev
RUN \
source /root/pyenvs/dev/bin/activate && \
/opt/bootstrap/withproxy pip --trusted-host pypi.org --trusted-host pypi.python.org --trusted-host files.pythonhosted.org install --upgrade pip && \
/opt/bootstrap/withproxy pip --trusted-host pypi.org --trusted-host pypi.python.org --trusted-host files.pythonhosted.org install -U setuptools ${REQUIREMENTS}

# Install Pegasus current and next venvs for debugging real libraries
ADD bootstrap/install_pegasus.sh /opt/bootstrap/
RUN /opt/bootstrap/install_pegasus.sh

ADD manylinux_entrypoint.sh /opt/bootstrap/
ENTRYPOINT /opt/bootstrap/manylinux_entrypoint.sh

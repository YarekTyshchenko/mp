#!/usr/bin/env python
"""
Set up build environment on Ubuntu or other Debian-based
Linux distribution.

Usage:
  bootstrap-linux.py [buildbot | docker]

buildbot: install buildbot slave
"""

import platform, re, shutil
from bootstrap import *
from subprocess import check_call, Popen, PIPE

if __name__ == '__main__':
  vagrant = bootstrap_init()

  import docopt
  args = docopt.docopt(__doc__)

  x86_64 = platform.architecture()[0] == '64bit'

  # Install build tools.
  if not installed('cmake'):
    # Install python-software-properties for apt-add-repository.
    check_call(['apt-get', 'install', '-qy', 'python-software-properties'])
    # Add git-core PPA for newer version of Git because version 1.7 available
    # in Lucid cannot access private repos on GitHub via a token.
    check_call(['add-apt-repository', 'ppa:git-core/ppa'])
    # Add webupd8team java PPA for Java 7.
    check_call(['add-apt-repository', 'ppa:webupd8team/java'])
    # Suppress license dialog.
    cmd = 'echo debconf shared/accepted-oracle-license-v1-1 {0} true | ' + \
          'debconf-set-selections'
    check_call(cmd.format('select'), shell=True)
    check_call(cmd.format('seen'), shell=True)
    # Install packages.
    check_call(['apt-get', 'update', '-q'])
    packages = [
      'git-core', 'gcc', 'g++', 'gfortran', 'ccache', 'make',
      'oracle-java7-installer', 'oracle-java7-set-default',
      'libgtk2.0-0', 'libxrender1', 'libxtst6', # Java/Eclipse requirements
      'python-dev', 'unixodbc-dev'
    ]
    if x86_64:
      packages.append('libc6-i386')
    check_call(['apt-get', 'install', '-qy'] + packages)

    install_cmake('cmake-3.1.0-Linux-i386.tar.gz')
    install_maven()

  # Installs symlinks for ccache.
  for name in ['gcc', 'cc', 'g++', 'c++']:
    add_to_path(which('ccache'), name)

  install_f90cache()
  output = Popen(['gfortran', '--version'], stdout=PIPE).communicate()[0]
  version = re.match(r'.* (\d+\.\d+)\.\d+', output).group(1)
  add_to_path('/usr/local/bin/f90cache', 'gfortran-' + version)

  docker = args['docker']
  if docker:
    # Install x11vnc 0.9.10 from maverick because version 0.9.9 from lucid is
    # broken: https://bugs.launchpad.net/ubuntu/+source/x11vnc/+bug/645106
    # x11vnc and miwm (a window manager) are required for GUI tests.
    with open('/etc/apt/apt.conf.d/01ubuntu', 'a') as f:
      f.write('\nAPT::Default-Release "lucid";\n')
    repo_url = 'http://old-releases.ubuntu.com/ubuntu'
    check_call(['add-apt-repository',
                'deb {0} maverick main universe'.format(repo_url)])
    check_call(['add-apt-repository',
                'deb {0} maverick-updates main universe'.format(repo_url)])
    check_call(['apt-get', 'update', '-q'])
    check_call(['apt-get', 'install', '-qy',
                'libssl0.9.8=0.9.8o-1ubuntu4.6', 'xvfb', 'x11vnc', 'xinit', 'miwm'])

  # Install LocalSolver.
  if not installed('localsolver'):
    ls_filename = 'LocalSolver_{0}_Linux{1}.run'.format(
      LOCALSOLVER_VERSION, 64 if x86_64 else 32)
    with download('http://www.localsolver.com/downloads/' + ls_filename) as f:
      check_call(['sh', f])

  copy_optional_dependencies('linux-' + platform.machine())

  if args['buildbot'] or docker:
    ip = '172.17.42.1' if docker else None
    path = install_buildbot_slave(
      'lucid64' if x86_64 else 'lucid32', ip=ip)
    if not docker and path:
      pip_install('python-crontab', 'crontab')
      from crontab import CronTab
      cron = CronTab(username)
      cron.new('PATH={0}:/usr/local/bin buildslave start {1}'.format(
        os.environ['PATH'], path)).every_reboot()
      cron.write()
      # Ignore errors from buildslave as the buildbot may not be accessible.
      call(['sudo', '-H', '-u', username, 'buildslave', 'start', path])


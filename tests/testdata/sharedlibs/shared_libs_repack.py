#  Copyright (C) 2020 The Android Open Source Project
#
#  Licensed under the Apache License, Version 2.0 (the "License");
#  you may not use this file except in compliance with the License.
#  You may obtain a copy of the License at
#
#       http://www.apache.org/licenses/LICENSE-2.0
#
#  Unless required by applicable law or agreed to in writing, software
#  distributed under the License is distributed on an "AS IS" BASIS,
#  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
#  See the License for the specific language governing permissions and
#  limitations under the License.
#
#  Licensed under the Apache License, Version 2.0 (the "License");
#  you may not use this file except in compliance with the License.
#  You may obtain a copy of the License at
#
#       http://www.apache.org/licenses/LICENSE-2.0
#
#  Unless required by applicable law or agreed to in writing, software
#  distributed under the License is distributed on an "AS IS" BASIS,
#  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
#  See the License for the specific language governing permissions and
#  limitations under the License.
"""Repacking tool for Shared Libs APEX testing."""

import argparse
import hashlib
import logging
import os
import shutil
import subprocess
import sys
import tempfile
from zipfile import ZipFile

import apex_build_info_pb2
import apex_manifest_pb2

logger = logging.getLogger(__name__)


def parse_args(argv):
  parser = argparse.ArgumentParser(
      description='Repacking tool for Shared Libs APEX testing')

  parser.add_argument('--input', required=True, help='Input file')
  parser.add_argument('--output', required=True, help='Output file')
  parser.add_argument(
      '--key', required=True, help='Path to the private avb key file')
  parser.add_argument(
      '--pk8key',
      required=True,
      help='Path to the private apk key file in pk8 format')
  parser.add_argument(
      '--pubkey', required=True, help='Path to the public avb key file')
  parser.add_argument(
      '--tmpdir', required=True, help='Temporary directory to use')
  parser.add_argument(
      '--x509key',
      required=True,
      help='Path to the public apk key file in x509 format')
  parser.add_argument(
      '--mode', default='strip', choices=['strip', 'sharedlibs'])
  return parser.parse_args(argv)


def run(args, verbose=None, **kwargs):
  """Creates and returns a subprocess.Popen object.

  Args:
    args: The command represented as a list of strings.
    verbose: Whether the commands should be shown. Default to the global
      verbosity if unspecified.
    kwargs: Any additional args to be passed to subprocess.Popen(), such as env,
      stdin, etc. stdout and stderr will default to subprocess.PIPE and
      subprocess.STDOUT respectively unless caller specifies any of them.
      universal_newlines will default to True, as most of the users in
      releasetools expect string output.

  Returns:
    A subprocess.Popen object.
  """
  if 'stdout' not in kwargs and 'stderr' not in kwargs:
    kwargs['stdout'] = subprocess.PIPE
    kwargs['stderr'] = subprocess.STDOUT
  if 'universal_newlines' not in kwargs:
    kwargs['universal_newlines'] = True
  if verbose:
    logger.info('  Running: \"%s\"', ' '.join(args))
  return subprocess.Popen(args, **kwargs)


def run_and_check_output(args, verbose=None, **kwargs):
  """Runs the given command and returns the output.

  Args:
    args: The command represented as a list of strings.
    verbose: Whether the commands should be shown. Default to the global
      verbosity if unspecified.
    kwargs: Any additional args to be passed to subprocess.Popen(), such as env,
      stdin, etc. stdout and stderr will default to subprocess.PIPE and
      subprocess.STDOUT respectively unless caller specifies any of them.

  Returns:
    The output string.

  Raises:
    ExternalError: On non-zero exit from the command.
  """
  proc = run(args, verbose=verbose, **kwargs)
  output, _ = proc.communicate()
  if output is None:
    output = ''
  # Don't log any if caller explicitly says so.
  if verbose:
    logger.info('%s', output.rstrip())
  if proc.returncode != 0:
    raise RuntimeError(
        'Failed to run command \'{}\' (exit code {}):\n{}'.format(
            args, proc.returncode, output))
  return output


def get_container_files(apex_file_path, tmpdir):
  dir_name = tempfile.mkdtemp(prefix='container_files_', dir=tmpdir)
  with ZipFile(apex_file_path, 'r') as zip_obj:
    zip_obj.extractall(path=dir_name)
  files = {}
  for i in [
      'apex_manifest.json', 'apex_manifest.pb', 'apex_build_info.pb', 'assets',
      'apex_payload.img', 'apex_payload.zip'
  ]:
    file_path = os.path.join(dir_name, i)
    if os.path.exists(file_path):
      files[i] = file_path

  image_file = files.get('apex_payload.img')
  if image_file is None:
    image_file = files.get('apex_payload.zip')

  files['apex_payload'] = image_file

  return files


def extract_payload_from_img(img_file_path, tmpdir):
  dir_name = tempfile.mkdtemp(prefix='extracted_payload_', dir=tmpdir)
  cmd = [
      _get_host_tools_path('debugfs_static'), '-R',
      'rdump ./ %s' % dir_name, img_file_path
  ]
  run_and_check_output(cmd)

  # Remove payload files added by apexer and e2fs tools.
  for i in ['apex_manifest.json', 'apex_manifest.pb']:
    if os.path.exists(os.path.join(dir_name, i)):
      os.remove(os.path.join(dir_name, i))
  if os.path.isdir(os.path.join(dir_name, 'lost+found')):
    shutil.rmtree(os.path.join(dir_name, 'lost+found'))
  return dir_name


def run_apexer(container_files, payload_dir, key_path, pubkey_path, tmpdir):
  apexer_cmd = _get_host_tools_path('apexer')
  cmd = [
      apexer_cmd, '--force', '--include_build_info', '--do_not_check_keyname'
  ]
  cmd.extend([
      '--apexer_tool_path',
      os.path.dirname(apexer_cmd) + ':prebuilts/sdk/tools/linux/bin'
  ])
  cmd.extend(['--manifest', container_files['apex_manifest.pb']])
  if 'apex_manifest.json' in container_files:
    cmd.extend(['--manifest_json', container_files['apex_manifest.json']])
  cmd.extend(['--build_info', container_files['apex_build_info.pb']])
  if 'assets' in container_files:
    cmd.extend(['--assets_dir', container_files['assets']])
  cmd.extend(['--key', key_path])
  cmd.extend(['--pubkey', pubkey_path])

  # Decide on output file name
  apex_suffix = '.apex.unsigned'
  fd, fn = tempfile.mkstemp(prefix='repacked_', suffix=apex_suffix, dir=tmpdir)
  os.close(fd)
  cmd.extend([payload_dir, fn])

  run_and_check_output(cmd)
  return fn


def _get_java_toolchain():
  java_toolchain = 'java'
  if os.path.isfile('prebuilts/jdk/jdk11/linux-x86/bin/java'):
    java_toolchain = 'prebuilts/jdk/jdk11/linux-x86/bin/java'

  java_dep_lib = (
      os.path.join(os.path.dirname(_get_host_tools_path()), 'lib64') + ':' +
      os.path.join(os.path.dirname(_get_host_tools_path()), 'lib'))

  return [java_toolchain, java_dep_lib]


def _get_host_tools_path(tool_name=None):
  # This script is located at e.g.
  # out/soong/host/linux-x86/bin/shared_libs_repack/shared_libs_repack.py.
  # Find the host tools dir by going up two directories.
  dirname = os.path.dirname(os.path.dirname(os.path.realpath(__file__)))
  if tool_name:
    return os.path.join(dirname, tool_name)
  return dirname


def sign_apk_container(unsigned_apex, x509key_path, pk8key_path, tmpdir):
  fd, fn = tempfile.mkstemp(prefix='repacked_', suffix='.apex', dir=tmpdir)
  os.close(fd)
  java_toolchain, java_dep_lib = _get_java_toolchain()

  cmd = [
      java_toolchain, '-Djava.library.path=' + java_dep_lib, '-jar',
      os.path.join(
          os.path.dirname(_get_host_tools_path()), 'framework', 'signapk.jar'),
      '-a', '4096', x509key_path, pk8key_path, unsigned_apex, fn
  ]
  run_and_check_output(cmd)
  return fn


def compute_sha512(file_path):
  block_size = 65536
  hashbuf = hashlib.sha512()
  with open(file_path, 'rb') as f:
    fb = f.read(block_size)
    while len(fb) > 0:
      hashbuf.update(fb)
      fb = f.read(block_size)
  return hashbuf.hexdigest()


def main(argv):
  args = parse_args(argv)
  apex_file_path = args.input

  container_files = get_container_files(apex_file_path, args.tmpdir)
  payload_dir = extract_payload_from_img(container_files['apex_payload.img'],
                                         args.tmpdir)
  print(payload_dir)
  libpath = 'lib64'
  libcpp = os.path.join(payload_dir, libpath, 'libc++.so')
  if not os.path.exists(libcpp):
    libpath = 'lib'
    libcpp = os.path.join(payload_dir, libpath, 'libc++.so')
  libcpp_hash = compute_sha512(libcpp)
  if args.mode == 'strip':
    # Stripping mode. Add a reference to the version of libc++.so to the
    # sharedApexLibs entry in the manifest, and remove lib64/libc++.so from
    # the payload.
    pb = apex_manifest_pb2.ApexManifest()
    with open(container_files['apex_manifest.pb'], 'rb') as f:
      pb.ParseFromString(f.read())
      pb.sharedApexLibs.append('libc++.so:' + libcpp_hash)
      #
      # Example of resulting manifest -- use  print(MessageToString(pb)) :
      # name: "com.android.apex.test.foo"
      # version: 1
      # requireNativeLibs: "libc.so"
      # requireNativeLibs: "libdl.so"
      # requireNativeLibs: "libm.so"
      # sharedApexLibs : "libc++.so:83d8f50..."
    with open(container_files['apex_manifest.pb'], 'wb') as f:
      f.write(pb.SerializeToString())
    # Replace existing library with symlink
    symlink_dst = os.path.join('/', 'apex',
                               'com.android.apex.test.sharedlibs',
                               libpath, 'libc++.so', libcpp_hash,
                               'libc++.so')
    os.remove(libcpp)
    os.system('ln -s {0} {1}'.format(symlink_dst, libcpp))

  if args.mode == 'sharedlibs':
    # We assume that libcpp exists in lib64/ or lib/. We'll move it to a
    # directory named lib/libc++.so/${SHA512_OF_LIBCPP}/
    #
    tmp_libcpp = os.path.join(payload_dir, libpath, 'libc++_backup.so')
    shutil.move(libcpp, tmp_libcpp)
    destdir = os.path.join(payload_dir, libpath, 'libc++.so', libcpp_hash)
    os.makedirs(destdir)
    shutil.move(tmp_libcpp, os.path.join(destdir, 'libc++.so'))

    pb = apex_build_info_pb2.ApexBuildInfo()
    with open(container_files['apex_build_info.pb'], 'rb') as f:
      pb.ParseFromString(f.read())

    canned_fs_config = pb.canned_fs_config.decode('utf-8')
    canned_fs_config += os.path.join('/', libpath, 'libc++.so',
                                     libcpp_hash, 'libc++.so') + \
                                     ' 1000 1000 0644\n'
    canned_fs_config += '/' + libpath + ' 0 2000 0755\n'
    canned_fs_config += '/' + libpath + '/libc++.so 0 2000 0755\n'
    canned_fs_config += '/' + libpath + '/libc++.so/' + libcpp_hash + \
                        ' 0 2000 0755\n'
    pb.canned_fs_config = canned_fs_config.encode('utf-8')
    with open(container_files['apex_build_info.pb'], 'wb') as f:
      f.write(pb.SerializeToString())

  try:
    os.rmdir(os.path.dirname(libcpp))
  except OSError:
    # Directory not empty, that's OK.
    pass

  repack_apex_file_path = run_apexer(container_files, payload_dir, args.key,
                                     args.pubkey, args.tmpdir)

  resigned_apex_file_path = sign_apk_container(repack_apex_file_path,
                                               args.x509key, args.pk8key,
                                               args.tmpdir)

  shutil.copyfile(resigned_apex_file_path, args.output)


if __name__ == '__main__':
  main(sys.argv[1:])

"""
aws_sync.py — AWS S3 Cloud Integration for HelixVault Object Storage

Syncs locally deduplicated physical chunks and object manifests to a real
Amazon Web Services (AWS) S3 bucket using the official AWS Boto3 SDK.

Features:
  - Supports Standard IAM Credentials & Temporary Session Tokens (Skill Builder Labs)
  - Uploads only UNIQUE deduplicated chunks (saving AWS S3 storage bills)
  - Real-time verification & listing of AWS S3 bucket objects
"""

import os
import sys
import json
import boto3
from botocore.exceptions import ClientError, NoCredentialsError

CONFIG_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "aws_config.json")
STORAGE_ROOT = os.getenv("HELIX_ROOT", os.path.join(os.path.dirname(os.path.abspath(__file__)), "storage_root"))


def load_config():
    """Load AWS configuration from config file or environment variables."""
    config = {
        "aws_access_key_id": os.getenv("AWS_ACCESS_KEY_ID", ""),
        "aws_secret_access_key": os.getenv("AWS_SECRET_ACCESS_KEY", ""),
        "aws_session_token": os.getenv("AWS_SESSION_TOKEN", ""),
        "region_name": os.getenv("AWS_DEFAULT_REGION", "us-east-1"),
        "s3_bucket_name": os.getenv("AWS_S3_BUCKET_NAME", "")
    }

    if os.path.exists(CONFIG_FILE):
        try:
            with open(CONFIG_FILE, "r") as f:
                saved = json.load(f)
                config.update(saved)
        except Exception as e:
            print(f"[Warning] Could not read {CONFIG_FILE}: {e}")

    return config


def save_config(config):
    """Save AWS credentials to local configuration file."""
    with open(CONFIG_FILE, "w") as f:
        json.dump(config, f, indent=4)
    print(f"\n[OK] AWS credentials saved successfully to: {CONFIG_FILE}")


def get_s3_client(config):
    """Create and return an authenticated boto3 S3 client."""
    kwargs = {"region_name": config.get("region_name") or "us-east-1"}

    if config.get("aws_access_key_id") and config.get("aws_secret_access_key"):
        kwargs["aws_access_key_id"] = config["aws_access_key_id"]
        kwargs["aws_secret_access_key"] = config["aws_secret_access_key"]

    if config.get("aws_session_token"):
        kwargs["aws_session_token"] = config["aws_session_token"]

    return boto3.client("s3", **kwargs)


def interactive_setup():
    """Interactive prompt to input AWS credentials."""
    print("\n=================================================================")
    print("           AWS S3 CLOUD CREDENTIALS CONFIGURATION                ")
    print("=================================================================")
    print("If using AWS Skill Builder Labs, copy the credentials from the")
    print("'AWS Details' / 'Credentials' tab inside the lab session.")
    print("-----------------------------------------------------------------")

    current = load_config()

    access_key = input(f"AWS Access Key ID [{current.get('aws_access_key_id', '')[:8]}...]: ").strip()
    if not access_key and current.get("aws_access_key_id"):
        access_key = current["aws_access_key_id"]

    secret_key = input(f"AWS Secret Access Key: ").strip()
    if not secret_key and current.get("aws_secret_access_key"):
        secret_key = current["aws_secret_access_key"]

    session_token = input("AWS Session Token (Press Enter if using permanent keys): ").strip()
    if not session_token and current.get("aws_session_token"):
        session_token = current["aws_session_token"]

    region = input(f"AWS Region [{current.get('region_name', 'us-east-1')}]: ").strip()
    if not region:
        region = current.get("region_name", "us-east-1")

    bucket = input(f"AWS S3 Bucket Name [{current.get('s3_bucket_name', '')}]: ").strip()
    if not bucket:
        bucket = current.get("s3_bucket_name", "")

    new_config = {
        "aws_access_key_id": access_key,
        "aws_secret_access_key": secret_key,
        "aws_session_token": session_token,
        "region_name": region,
        "s3_bucket_name": bucket
    }

    save_config(new_config)
    print("\nTesting connection to AWS S3...")
    test_connection(new_config)


def test_connection(config=None):
    """Test connectivity to AWS S3 and check if the bucket is accessible."""
    if config is None:
        config = load_config()

    bucket = config.get("s3_bucket_name")
    if not bucket:
        print("[Error] No S3 Bucket specified. Run 'python aws_sync.py setup' first.")
        return False

    try:
        s3 = get_s3_client(config)
        # Try head_bucket to verify existence and access
        s3.head_bucket(Bucket=bucket)
        print(f"[SUCCESS] Connected to Real AWS S3 Bucket: '{bucket}' (Region: {config.get('region_name')})")
        return True
    except ClientError as e:
        error_code = e.response.get("Error", {}).get("Code", "")
        if error_code == "404":
            print(f"[Notice] Bucket '{bucket}' does not exist on AWS yet.")
            create = input(f"Would you like to create bucket '{bucket}' now on AWS S3? (y/n): ").strip().lower()
            if create == 'y':
                return create_bucket(config, bucket)
        elif error_code == "403":
            print(f"[Error] Access Denied to bucket '{bucket}'. Check your IAM permissions.")
        else:
            print(f"[Error] AWS ClientError: {e}")
        return False
    except NoCredentialsError:
        print("[Error] No AWS credentials found. Run 'python aws_sync.py setup' to configure.")
        return False
    except Exception as e:
        print(f"[Error] Could not connect to AWS: {e}")
        return False


def create_bucket(config, bucket_name):
    """Create a new bucket on AWS S3."""
    try:
        s3 = get_s3_client(config)
        region = config.get("region_name", "us-east-1")
        if region == "us-east-1":
            s3.create_bucket(Bucket=bucket_name)
        else:
            s3.create_bucket(
                Bucket=bucket_name,
                CreateBucketConfiguration={"LocationConstraint": region}
            )
        print(f"[SUCCESS] AWS S3 Bucket '{bucket_name}' created successfully on Amazon Cloud!")
        return True
    except Exception as e:
        print(f"[Error] Failed to create bucket '{bucket_name}': {e}")
        return False


def sync_to_aws(config=None, filter_local_bucket=None):
    """
    Scans storage_root/chunks and storage_root/buckets and uploads
    unique deduplicated chunks and manifests to the AWS S3 cloud.
    """
    if config is None:
        config = load_config()

    s3_bucket = config.get("s3_bucket_name")
    if not s3_bucket:
        print("[Error] No target AWS S3 bucket configured in aws_config.json. Run setup first.")
        return False

    s3 = get_s3_client(config)

    chunks_dir = os.path.join(STORAGE_ROOT, "chunks")
    buckets_dir = os.path.join(STORAGE_ROOT, "buckets")

    if not os.path.exists(chunks_dir):
        print(f"[Notice] No chunks directory found at '{chunks_dir}'. Upload some files in HelixVault first!")
        return False

    print("\n=================================================================")
    print("           AWS S3 HYBRID CLOUD DEDUPLICATION SYNC                ")
    print("=================================================================")
    print(f" Target Cloud Bucket : s3://{s3_bucket}/")
    print(f" Local Storage Root  : {STORAGE_ROOT}")
    if filter_local_bucket:
        print(f" Filter Local Bucket : {filter_local_bucket}")
    print("-----------------------------------------------------------------")

    # 1. Sync Unique Deduplicated Chunks
    uploaded_chunks = 0
    total_chunk_bytes = 0

    print("\n[Phase 1] Syncing Deduplicated Physical Chunks to AWS Cloud:")
    for shard in os.listdir(chunks_dir):
        shard_path = os.path.join(chunks_dir, shard)
        if not os.path.isdir(shard_path):
            continue

        for chunk_file in os.listdir(shard_path):
            chunk_full_path = os.path.join(shard_path, chunk_file)
            if not os.path.isfile(chunk_full_path):
                continue

            file_size = os.path.getsize(chunk_full_path)
            s3_key = f"chunks/{shard}/{chunk_file}"

            # Check if chunk already exists in S3 (avoid re-uploading)
            try:
                s3.head_object(Bucket=s3_bucket, Key=s3_key)
                print(f"  [EXISTS] {s3_key} (Already on Cloud)")
            except ClientError:
                # Chunk doesn't exist on AWS S3, upload it!
                print(f"  [UPLOAD] {s3_key} ({file_size} B) -> AWS S3 ...", end="", flush=True)
                s3.upload_file(chunk_full_path, s3_bucket, s3_key)
                print(" [OK]")
                uploaded_chunks += 1

            total_chunk_bytes += file_size

    # 2. Sync Object Manifests (Logical Files)
    uploaded_manifests = 0
    total_manifests = 0

    if os.path.exists(buckets_dir):
        print("\n[Phase 2] Syncing Logical Bucket Manifests:")
        for b_name in os.listdir(buckets_dir):
            if filter_local_bucket and b_name != filter_local_bucket:
                continue
            b_path = os.path.join(buckets_dir, b_name)
            if not os.path.isdir(b_path):
                continue

            for root, _, files in os.walk(b_path):
                for f in files:
                    if f.endswith(".manifest"):
                        total_manifests += 1
                        m_full = os.path.join(root, f)
                        rel_path = os.path.relpath(m_full, buckets_dir)
                        s3_key = f"buckets/{rel_path}".replace("\\", "/")

                        try:
                            s3.head_object(Bucket=s3_bucket, Key=s3_key)
                            print(f"  [EXISTS] {s3_key} (Already on Cloud)")
                        except ClientError:
                            print(f"  [UPLOAD] {s3_key} -> AWS S3 ...", end="", flush=True)
                            s3.upload_file(m_full, s3_bucket, s3_key)
                            print(" [OK]")
                            uploaded_manifests += 1

    print("\n-----------------------------------------------------------------")
    print(f" [SUCCESS] AWS S3 Cloud Sync Completed!")
    print(f" Total Unique Chunks Synced   : {uploaded_chunks} newly uploaded")
    print(f" Total Logical Files Managed  : {total_manifests}")
    print(f" Real Cloud Impact            : Only unique deduplicated blocks are stored")
    print(f"                                on AWS S3, reducing cloud costs!")
    print("=================================================================\n")
    return True


def list_aws_objects(config=None, target_bucket=None):
    """List all objects currently stored in the real AWS S3 bucket."""
    if config is None:
        config = load_config()

    s3_bucket = target_bucket or config.get("s3_bucket_name")
    if not s3_bucket:
        print("[Error] No S3 Bucket specified.")
        return

    s3 = get_s3_client(config)

    print(f"\n--- Objects in AWS S3 Bucket: s3://{s3_bucket}/ ---")
    try:
        resp = s3.list_objects_v2(Bucket=s3_bucket)
        contents = resp.get("Contents", [])
        if not contents:
            print("  (Bucket is currently empty)")
            return

        print(f"{'Key':<55} | {'Size':>10} | {'Last Modified'}")
        print("-" * 90)
        for obj in contents:
            key = obj['Key']
            size = f"{obj['Size']} B"
            mtime = str(obj['LastModified'])[:19]
            print(f"{key:<55} | {size:>10} | {mtime}")
        print("-" * 90)
        print(f"Total Objects in Cloud: {len(contents)}\n")
    except Exception as e:
        print(f"[Error] Failed to list objects in bucket '{s3_bucket}': {e}\n")


def print_help():
    print("""
Usage: python aws_sync.py <command> [args...]

Commands:
  setup           Configure AWS Access Key, Secret Key, Session Token, and Bucket Name
  test            Test connectivity to your AWS S3 bucket
  sync [bucket]   Sync local deduplicated chunks and manifests to AWS S3 Cloud
  list [bucket]   List all objects currently in the AWS S3 Cloud Bucket
  create <name>   Create a new AWS S3 Bucket directly on the Amazon cloud
""")


def main():
    if len(sys.argv) < 2:
        print_help()
        return

    cmd = sys.argv[1].lower()
    config = load_config()

    if cmd == "setup":
        interactive_setup()
    elif cmd == "test":
        test_connection(config)
    elif cmd == "sync":
        bucket = sys.argv[2] if len(sys.argv) > 2 else None
        sync_to_aws(config, bucket)
    elif cmd == "list":
        bucket = sys.argv[2] if len(sys.argv) > 2 else None
        list_aws_objects(config, bucket)
    elif cmd == "create":
        if len(sys.argv) < 3:
            print("Usage: python aws_sync.py create <bucket_name>")
            return
        create_bucket(config, sys.argv[2])
    else:
        print_help()


if __name__ == "__main__":
    main()


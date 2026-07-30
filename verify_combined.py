import sys
import os
import struct
import time
import numpy as np

TOLERANCE = 1e-2

# ================================================================
#  PHAN 1: KIEM CHUNG DATAFLOW (OS / WS / IS vs PyTorch)
# ================================================================
def verify_dataflow():
    import torch
    import torch.nn.functional as F

    print("=" * 60)
    print(" KIEM CHUNG DATAFLOW (OS / WS / IS vs PyTorch)")
    print("=" * 60)

    try:
        with open("config.txt") as f: lines = f.read().split()
        N = int(lines[0]); M = int(lines[1]); C = int(lines[2]); H = int(lines[3])
        R = int(lines[4]); S = int(lines[5]); E = int(lines[6])
    except FileNotFoundError:
        sys.exit("ERROR: config.txt not found. Chay ./cnn_analyzer dataflow truoc!")

    def read_floats(path):
        with open(path) as f: return list(map(float, f.readlines()))

    ifmap    = torch.tensor(read_floats("ifmap.txt")).reshape(N, C, H, H)
    filter_w = torch.tensor(read_floats("filter.txt")).reshape(M, C, R, R)
    t_os     = torch.tensor(read_floats("ofmap_c.txt")).reshape(N, M, E, E)
    t_ws     = torch.tensor(read_floats("ofmap_ws.txt")).reshape(N, M, E, E)
    t_is     = torch.tensor(read_floats("ofmap_is.txt")).reshape(N, M, E, E)

    ref = F.conv2d(ifmap, filter_w, stride=S, padding=0)

    all_pass = True
    for name, t in [("OS",t_os),("WS",t_ws),("IS",t_is)]:
        max_err = float(torch.abs(t - ref).max())
        ok = max_err < TOLERANCE
        if not ok: all_pass = False
        print(f" {name:<10} Max Err: {max_err:>12.4e} -> {'PASS' if ok else 'FAIL'}")

# ================================================================
#  PHAN 2: EXPORT WEIGHT CHO FULL NETWORK (AlexNet CIFAR-10)
# ================================================================
def export_weights():
    import torch
    import torch.nn as nn
    import torch.optim as optim
    import torchvision
    import torchvision.transforms as transforms

    print("\n" + "=" * 60)
    print(" EXPORT WEIGHT CHO ALEXNET CIFAR-10")
    print("=" * 60)

    os.makedirs("export_bin", exist_ok=True)

    class TinyCNN(nn.Module):
        def __init__(self):
            super().__init__()
            self.conv1 = nn.Conv2d(3, 32, 3, padding=1)
            self.conv2 = nn.Conv2d(32, 64, 3, padding=1)
            self.conv3 = nn.Conv2d(64, 128, 3, padding=1)
            self.conv4 = nn.Conv2d(128, 128, 3, padding=1)
            self.conv5 = nn.Conv2d(128, 128, 3, padding=1)
            self.pool  = nn.MaxPool2d(2, 2)
            self.fc1   = nn.Linear(128*4*4, 512)
            self.fc2   = nn.Linear(512, 256)
            self.fc3   = nn.Linear(256, 10)
            self.relu  = nn.ReLU()

        def forward(self, x):
            x = self.pool(self.relu(self.conv1(x)))
            x = self.pool(self.relu(self.conv2(x)))
            x = self.relu(self.conv3(x))
            x = self.relu(self.conv4(x))
            x = self.pool(self.relu(self.conv5(x)))
            x = x.view(x.size(0), -1)
            x = self.relu(self.fc1(x))
            x = self.relu(self.fc2(x))
            x = self.fc3(x)
            return x

    device = 'cpu'
    model = TinyCNN().to(device)
    ckpt_path = "export_bin/model.pth"

    if os.path.exists(ckpt_path):
        model.load_state_dict(torch.load(ckpt_path, map_location=device))
    else:
        transform = transforms.Compose([transforms.ToTensor(), transforms.Normalize((0.5,0.5,0.5),(0.5,0.5,0.5))])
        trainset = torchvision.datasets.CIFAR10(root="./data", train=True, download=True, transform=transform)
        trainloader = torch.utils.data.DataLoader(trainset, batch_size=128, shuffle=True)
        criterion = nn.CrossEntropyLoss()
        optimizer = optim.Adam(model.parameters(), lr=1e-3)

        for epoch in range(5):
            model.train()
            running_loss = 0.0
            for i, (inputs, labels) in enumerate(trainloader):
                optimizer.zero_grad()
                loss = criterion(model(inputs), labels)
                loss.backward()
                optimizer.step()
                running_loss += loss.item()
                if i % 100 == 99:
                    print(f"  Epoch {epoch+1} step {i+1} loss={running_loss/100:.3f}")
                    running_loss = 0.0
        torch.save(model.state_dict(), ckpt_path)

    model.eval()
    def save_bin(tensor, path):
        tensor.detach().cpu().numpy().astype(np.float32).tofile(path)

    for i, layer in enumerate([model.conv1,model.conv2,model.conv3,model.conv4,model.conv5], 1):
        save_bin(layer.weight, f"export_bin/conv{i}.weight.bin")
        save_bin(layer.bias,   f"export_bin/conv{i}.bias.bin")

    for i, layer in enumerate([model.fc1,model.fc2,model.fc3], 1):
        save_bin(layer.weight, f"export_bin/fc{i}.weight.bin")
        save_bin(layer.bias,   f"export_bin/fc{i}.bias.bin")

    transform_test = transforms.Compose([transforms.ToTensor(), transforms.Normalize((0.5,0.5,0.5),(0.5,0.5,0.5))])
    testset = torchvision.datasets.CIFAR10(root="./data", train=False, download=True, transform=transform_test)
    sample_img, _ = testset[0]
    sample_img.numpy().astype(np.float32).tofile("export_bin/sample_input.raw")

if __name__ == "__main__":
    mode = sys.argv[1] if len(sys.argv) > 1 else "all"
    if mode in ("verify", "all"): verify_dataflow()
    if mode in ("export", "all"): export_weights()